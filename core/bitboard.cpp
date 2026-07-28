#include "bitboard.h"

#include <cassert>
#include <cstdint>
#include <cstring>

// Magic bitboard attack tables.
//
// Layout: `rook_table` / `bishop_table` are one flat arena. Each square owns a
// contiguous slice of size 1<<popcount(mask), and `Magic::attacks` points at the
// start of that slice. All indexing is relative to `attacks` -- see
// Magic::index(). This is what the previous implementation got wrong: it stored
// `64 - bits` in `shift` and then used `shift` as a *table offset* on the PEXT
// path, so every lookup read from an unrelated square's slice.
//
// Two indexing schemes are supported and the table is filled to match whichever
// one is compiled in:
//   USE_PEXT  -> index = _pext_u64(occ, mask), which equals the ordinal of the
//                occupancy subset, so we fill by enumeration index directly.
//   otherwise -> index = ((occ & mask) * magic) >> shift, so we must first find
//                a `magic` that is collision-free for this square, then fill by
//                magic index.
//
// Either way init() ends with a full brute-force verification pass against the
// slow ray-walking generators; a bad table aborts at startup rather than
// silently corrupting search.

namespace Bitboard {

using chess::file_of;
using chess::rank_of;

uint64_t rook_table[102400];
uint64_t bishop_table[5248];
Magic rook_magics[64];
Magic bishop_magics[64];

uint64_t knight_attacks[64];
uint64_t king_attacks[64];
uint64_t pawn_attacks[2][64];

namespace {

constexpr int make_sq(int f, int r) { return r * 8 + f; }
constexpr uint64_t sq_bb(int s) { return 1ULL << s; }

// Relevant-occupancy masks exclude the board edge: a blocker on the last square
// of a ray does not change which squares are attacked.
uint64_t rook_mask(int sq) {
  uint64_t m = 0;
  const int r = rank_of(sq), f = file_of(sq);
  for (int rr = r + 1; rr <= 6; ++rr) m |= sq_bb(make_sq(f, rr));
  for (int rr = r - 1; rr >= 1; --rr) m |= sq_bb(make_sq(f, rr));
  for (int ff = f + 1; ff <= 6; ++ff) m |= sq_bb(make_sq(ff, r));
  for (int ff = f - 1; ff >= 1; --ff) m |= sq_bb(make_sq(ff, r));
  return m;
}

uint64_t bishop_mask(int sq) {
  uint64_t m = 0;
  const int r = rank_of(sq), f = file_of(sq);
  for (int d = 1; r + d <= 6 && f + d <= 6; ++d) m |= sq_bb(make_sq(f + d, r + d));
  for (int d = 1; r + d <= 6 && f - d >= 1; ++d) m |= sq_bb(make_sq(f - d, r + d));
  for (int d = 1; r - d >= 1 && f + d <= 6; ++d) m |= sq_bb(make_sq(f + d, r - d));
  for (int d = 1; r - d >= 1 && f - d >= 1; ++d) m |= sq_bb(make_sq(f - d, r - d));
  return m;
}

// Ground truth: walk each ray until (and including) the first blocker.
uint64_t slide(int sq, uint64_t occ, const int (*dirs)[2], int ndirs) {
  uint64_t att = 0;
  const int r = rank_of(sq), f = file_of(sq);
  for (int i = 0; i < ndirs; ++i) {
    const int dr = dirs[i][0], df = dirs[i][1];
    for (int rr = r + dr, ff = f + df; rr >= 0 && rr < 8 && ff >= 0 && ff < 8; rr += dr, ff += df) {
      const int s = make_sq(ff, rr);
      att |= sq_bb(s);
      if (occ & sq_bb(s)) break;
    }
  }
  return att;
}

const int ROOK_DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int BISHOP_DIRS[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

uint64_t rook_slow(int sq, uint64_t occ) { return slide(sq, occ, ROOK_DIRS, 4); }
uint64_t bishop_slow(int sq, uint64_t occ) { return slide(sq, occ, BISHOP_DIRS, 4); }

// Expand the i-th subset of `mask` (i in [0, 1<<popcount(mask))).
uint64_t subset(uint64_t mask, unsigned i) {
  uint64_t occ = 0;
  unsigned bit = 0;
  uint64_t m = mask;
  while (m) {
    const int s = __builtin_ctzll(m);
    m &= m - 1;
    if (i & (1u << bit)) occ |= sq_bb(s);
    ++bit;
  }
  return occ;
}

// Deterministic sparse PRNG (xorshift64star). Sparse candidates -- the AND of
// three draws -- have far fewer set bits and hit a valid magic much sooner.
struct Rng {
  uint64_t s = 0x246C'CB2D'3B40'2853ULL;
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 2685821657736338717ULL;
  }
  uint64_t sparse() { return next() & next() & next(); }
};

// Fill one square's slice, deriving a collision-free magic when not using PEXT.
void build(int sq, uint64_t mask, uint64_t* slice, Magic& out,
           uint64_t (*slow)(int, uint64_t), Rng& rng) {
  const int bits = __builtin_popcountll(mask);
  const unsigned size = 1u << bits;

  out.mask = mask;
  out.attacks = slice;
  out.shift = 64u - static_cast<unsigned>(bits);

  // Precompute (occupancy, attack) for every relevant subset.
  static uint64_t occs[4096], refs[4096];
  for (unsigned i = 0; i < size; ++i) {
    occs[i] = subset(mask, i);
    refs[i] = slow(sq, occs[i]);
  }

#if defined(USE_PEXT)
  out.magic = 0;
  // _pext_u64(occs[i], mask) == i by construction, so fill in order.
  for (unsigned i = 0; i < size; ++i) slice[i] = refs[i];
#else
  // Randomised search for a magic that is injective on attack sets. A collision
  // is tolerated only when both occupancies yield the identical attack set
  // ("constructive collision").
  static unsigned epoch[4096];
  static unsigned cur = 0;
  std::memset(epoch, 0, sizeof(epoch));
  cur = 0;

  for (;;) {
    const uint64_t magic = rng.sparse();
    // Cheap reject: a usable magic scatters the high bits of the mask.
    if (__builtin_popcountll((mask * magic) >> 56) < 6) continue;

    ++cur;
    bool ok = true;
    for (unsigned i = 0; i < size; ++i) {
      const unsigned idx = static_cast<unsigned>((occs[i] * magic) >> out.shift);
      if (idx >= size) { ok = false; break; }
      if (epoch[idx] == cur) {
        if (slice[idx] != refs[i]) { ok = false; break; }  // destructive
      } else {
        epoch[idx] = cur;
        slice[idx] = refs[i];
      }
    }
    if (ok) { out.magic = magic; break; }
  }
#endif
}

bool initialized = false;

}  // namespace

void init() {
  if (initialized) return;

  for (int sq = 0; sq < 64; ++sq) {
    const int r = rank_of(sq), f = file_of(sq);

    uint64_t n = 0;
    const int NDR[8] = {1, 2, 2, 1, -1, -2, -2, -1};
    const int NDF[8] = {2, 1, -1, -2, -2, -1, 1, 2};
    for (int i = 0; i < 8; ++i) {
      const int nr = r + NDR[i], nf = f + NDF[i];
      if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) n |= sq_bb(make_sq(nf, nr));
    }
    knight_attacks[sq] = n;

    uint64_t k = 0;
    for (int dr = -1; dr <= 1; ++dr)
      for (int df = -1; df <= 1; ++df) {
        if (!dr && !df) continue;
        const int nr = r + dr, nf = f + df;
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) k |= sq_bb(make_sq(nf, nr));
      }
    king_attacks[sq] = k;

    uint64_t wp = 0, bp = 0;
    if (r < 7) {
      if (f > 0) wp |= sq_bb(make_sq(f - 1, r + 1));
      if (f < 7) wp |= sq_bb(make_sq(f + 1, r + 1));
    }
    if (r > 0) {
      if (f > 0) bp |= sq_bb(make_sq(f - 1, r - 1));
      if (f < 7) bp |= sq_bb(make_sq(f + 1, r - 1));
    }
    pawn_attacks[0][sq] = wp;
    pawn_attacks[1][sq] = bp;
  }

  Rng rng;
  unsigned roff = 0, boff = 0;
  for (int sq = 0; sq < 64; ++sq) {
    const uint64_t rm = rook_mask(sq);
    build(sq, rm, &rook_table[roff], rook_magics[sq], rook_slow, rng);
    roff += 1u << __builtin_popcountll(rm);

    const uint64_t bm = bishop_mask(sq);
    build(sq, bm, &bishop_table[boff], bishop_magics[sq], bishop_slow, rng);
    boff += 1u << __builtin_popcountll(bm);
  }
  assert(roff == 102400 && "rook arena size mismatch");
  assert(boff == 5248 && "bishop arena size mismatch");

  initialized = true;

  // Verify every square against every relevant occupancy. Cheap (~200k probes)
  // and turns a silent table bug into an immediate, loud startup failure.
  const bool ok = verify();
  (void)ok;
  assert(ok && "magic bitboard verification failed");
}

bool verify() {
  for (int sq = 0; sq < 64; ++sq) {
    const uint64_t rm = rook_magics[sq].mask;
    const unsigned rn = 1u << __builtin_popcountll(rm);
    for (unsigned i = 0; i < rn; ++i) {
      const uint64_t occ = subset(rm, i);
      if (rook_attacks(sq, occ) != rook_slow(sq, occ)) return false;
    }
    const uint64_t bm = bishop_magics[sq].mask;
    const unsigned bn = 1u << __builtin_popcountll(bm);
    for (unsigned i = 0; i < bn; ++i) {
      const uint64_t occ = subset(bm, i);
      if (bishop_attacks(sq, occ) != bishop_slow(sq, occ)) return false;
    }
  }
  return true;
}

}  // namespace Bitboard
