#include "bitboard.h"
#include <cstring>
#include <cstdint>
#include <random>
#include <vector>

typedef uint64_t U64;
using chess::file_of;
using chess::rank_of;
static constexpr int  make_sq(int f, int r) { return r * 8 + f; }
static constexpr U64  square_bb(int s)      { return 1ULL << s; }

namespace Bitboard {

U64 rook_table[102400];
U64 bishop_table[5248];
Magic rook_magics[64];
Magic bishop_magics[64];

U64 knight_attacks[64];
U64 king_attacks[64];
U64 pawn_attacks[2][64];
U64 between_bb[64][64];
U64 line_bb[64][64];

static int popcnt(U64 x) { return __builtin_popcountll(x); }

static U64 generate_rook_mask(int sq) {
  U64 mask = 0; int r = rank_of(sq), f = file_of(sq);
  for (int rr = r + 1; rr <= 6; ++rr) mask |= square_bb(make_sq(f, rr));
  for (int rr = r - 1; rr >= 1; --rr) mask |= square_bb(make_sq(f, rr));
  for (int ff = f + 1; ff <= 6; ++ff) mask |= square_bb(make_sq(ff, r));
  for (int ff = f - 1; ff >= 1; --ff) mask |= square_bb(make_sq(ff, r));
  return mask;
}
static U64 generate_bishop_mask(int sq) {
  U64 mask = 0; int r = rank_of(sq), f = file_of(sq);
  for (int d = 1; r + d <= 6 && f + d <= 6; ++d) mask |= square_bb(make_sq(f + d, r + d));
  for (int d = 1; r + d <= 6 && f - d >= 1; ++d) mask |= square_bb(make_sq(f - d, r + d));
  for (int d = 1; r - d >= 1 && f + d <= 6; ++d) mask |= square_bb(make_sq(f + d, r - d));
  for (int d = 1; r - d >= 1 && f - d >= 1; ++d) mask |= square_bb(make_sq(f - d, r - d));
  return mask;
}
static U64 slide(int sq, U64 block, const int dirs[4][2]) {
  U64 att = 0; int r = rank_of(sq), f = file_of(sq);
  for (int i = 0; i < 4; ++i) {
    int nf = f + dirs[i][0], nr = r + dirs[i][1];
    while (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) {
      int s = make_sq(nf, nr);
      att |= square_bb(s);
      if (block & square_bb(s)) break;
      nf += dirs[i][0]; nr += dirs[i][1];
    }
  }
  return att;
}
static const int ROOK_DIRS[4][2]   = {{1,0},{-1,0},{0,1},{0,-1}};
static const int BISHOP_DIRS[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};

U64 rook_attacks_slow(int sq, U64 block)   { return slide(sq, block, ROOK_DIRS); }
U64 bishop_attacks_slow(int sq, U64 block) { return slide(sq, block, BISHOP_DIRS); }

extern const U64 ROOK_MAGICS_NUM[64];
extern const U64 BISHOP_MAGICS_NUM[64];

// Fill one square's attack table. Returns true when the chosen index scheme is
// collision free. When BMI2 is available we index by PEXT (always bijective);
// otherwise we index by magic multiply and verify - falling back to a magic
// search if the hardcoded constant does not work for this mask.
static bool build_square(U64 mask, U64 magic, unsigned shift, U64* table, int sq, bool rook) {
  const int bits = popcnt(mask);
  const int size = 1 << bits;
  int bitpos[16]; int nb = 0;
  for (int i = 0; i < 64; ++i) if ((mask >> i) & 1ULL) bitpos[nb++] = i;

  std::vector<char> used(size, 0);
  for (int i = 0; i < size; ++i) {
    U64 occ = 0;
    for (int j = 0; j < bits; ++j) if ((i >> j) & 1) occ |= 1ULL << bitpos[j];
    U64 att = rook ? rook_attacks_slow(sq, occ) : bishop_attacks_slow(sq, occ);
#if defined(USE_PEXT) && defined(__BMI2__)
    (void)magic; (void)shift;
    size_t idx = (size_t)i;               // enumeration index == PEXT index
#else
    size_t idx = (size_t)(((occ & mask) * magic) >> shift);
    if (idx >= (size_t)size) return false;
    if (used[idx] && table[idx] != att) return false;   // real collision
#endif
    used[idx] = 1;
    table[idx] = att;
  }
  return true;
}

static U64 sparse_random(std::mt19937_64& rng) {
  return rng() & rng() & rng();
}

void init() {
  static bool done = false;
  if (done) return;
  done = true;

  for (int sq = 0; sq < 64; ++sq) {
    int r = rank_of(sq), f = file_of(sq);
    U64 att = 0;
    const int drN[8] = {1,2,2,1,-1,-2,-2,-1};
    const int dfN[8] = {2,1,-1,-2,-2,-1,1,2};
    for (int i = 0; i < 8; ++i) {
      int nr = r + drN[i], nf = f + dfN[i];
      if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) att |= square_bb(make_sq(nf, nr));
    }
    knight_attacks[sq] = att;

    U64 katt = 0;
    for (int dr = -1; dr <= 1; ++dr) for (int df = -1; df <= 1; ++df) if (dr || df) {
      int nr = r + dr, nf = f + df;
      if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) katt |= square_bb(make_sq(nf, nr));
    }
    king_attacks[sq] = katt;

    U64 wp = 0, bp = 0;
    if (r < 7) { if (f > 0) wp |= square_bb(make_sq(f-1, r+1)); if (f < 7) wp |= square_bb(make_sq(f+1, r+1)); }
    if (r > 0) { if (f > 0) bp |= square_bb(make_sq(f-1, r-1)); if (f < 7) bp |= square_bb(make_sq(f+1, r-1)); }
    pawn_attacks[0][sq] = wp;
    pawn_attacks[1][sq] = bp;
  }

  std::mt19937_64 rng(0xDEADBEEFCAFEULL);
  size_t roff = 0, boff = 0;
  for (int sq = 0; sq < 64; ++sq) {
    U64 mask = generate_rook_mask(sq);
    int bits = popcnt(mask);
    rook_magics[sq].mask    = mask;
    rook_magics[sq].shift   = 64 - bits;
    rook_magics[sq].attacks = &rook_table[roff];
    U64 magic = ROOK_MAGICS_NUM[sq];
    while (!build_square(mask, magic, 64 - bits, &rook_table[roff], sq, true)) {
      magic = sparse_random(rng);
    }
    rook_magics[sq].magic = magic;
    roff += (size_t)1 << bits;

    U64 bmask = generate_bishop_mask(sq);
    int bbits = popcnt(bmask);
    bishop_magics[sq].mask    = bmask;
    bishop_magics[sq].shift   = 64 - bbits;
    bishop_magics[sq].attacks = &bishop_table[boff];
    U64 bmagic = BISHOP_MAGICS_NUM[sq];
    while (!build_square(bmask, bmagic, 64 - bbits, &bishop_table[boff], sq, false)) {
      bmagic = sparse_random(rng);
    }
    bishop_magics[sq].magic = bmagic;
    boff += (size_t)1 << bbits;
  }

  for (int a = 0; a < 64; ++a) for (int b = 0; b < 64; ++b) {
    between_bb[a][b] = 0; line_bb[a][b] = 0;
    if (a == b) continue;
    for (int i = 0; i < 4; ++i) {
      const int (*dirs)[2] = (i < 2) ? nullptr : nullptr; (void)dirs;
    }
    U64 ra = rook_attacks_slow(a, 0), ba = bishop_attacks_slow(a, 0);
    if (ra & square_bb(b)) {
      between_bb[a][b] = rook_attacks_slow(a, square_bb(b)) & rook_attacks_slow(b, square_bb(a));
      line_bb[a][b]    = (ra & rook_attacks_slow(b, 0)) | square_bb(a) | square_bb(b);
    } else if (ba & square_bb(b)) {
      between_bb[a][b] = bishop_attacks_slow(a, square_bb(b)) & bishop_attacks_slow(b, square_bb(a));
      line_bb[a][b]    = (ba & bishop_attacks_slow(b, 0)) | square_bb(a) | square_bb(b);
    }
  }
}
const U64 ROOK_MAGICS_NUM[64] = {
0x8a80104000800020ULL,
0x140002000100040ULL,
0x2801880a0017001ULL,
0x100081001000420ULL,
0x200020010080420ULL,
0x3001c0002010008ULL,
0x8480008002000100ULL,
0x2080088004402900ULL,
0x800098204000ULL,
0x2024401000200040ULL,
0x100802000801000ULL,
0x120800800801000ULL,
0x208808088000400ULL,
0x2802200800400ULL,
0x2200800100020080ULL,
0x801000060821100ULL,
0x80044006422000ULL,
0x100808020004000ULL,
0x12108a0010204200ULL,
0x140848010000802ULL,
0x4810200800400ULL,
0x14880008358000ULL,
0x80002800400ULL,
0x80008040006000ULL,
0x400080800400ULL,
0x8001000200004080ULL,
0x121000840800200ULL,
0x11a000800080104ULL,
0x2208080a800400ULL,
0x604000800840110ULL,
0x200200202002d000ULL,
0x1002100202048600ULL,
0x4002020a04200ULL,
0x8208089600000100ULL,
0x1004000204008200ULL,
0x1004808100060240ULL,
0x22220a008080004ULL,
0x2020081000a00108ULL,
0x8001000250008002ULL,
0x2800800400c0c100ULL,
0x2020880028001040ULL,
0x110100020a020080ULL,
0x10008818020200ULL,
0x2000100080080800ULL,
0x80808100100150ULL,
0x4000804000802000ULL,
0x8000100880010080ULL,
0x20020800200408ULL,
0x8120104000802000ULL,
0x84080808800ULL,
0x8008800400480100ULL,
0x1001080008a008a0ULL,
0x4000800080010ULL,
0x8000400080022004ULL,
0x4000800000800ULL,
0x8008000410080080ULL,
0x402008008080040ULL,
0x14002008240802ULL,
0x48a080000020a0ULL,
0x104a080004000020ULL,
0x80a080010008008ULL,
0x81001000800a0880ULL,
0x8002800080202400ULL,
0x8008201820140040ULL
};
const U64 BISHOP_MAGICS_NUM[64] = {
0x40040844404084ULL,
0x2004208a004208ULL,
0x10190041080202ULL,
0x108060845042010ULL,
0x581104180800210ULL,
0x2112080446200010ULL,
0x1080820820060210ULL,
0x3c0808410220200ULL,
0x4050404440404ULL,
0x21001420088ULL,
0x24d0080801082102ULL,
0x1020a0a020400ULL,
0x40308200402ULL,
0x80a02a41044000ULL,
0x8312260014001ULL,
0x800602000423ULL,
0xa000a0004e4050ULL,
0x4108004a03104ULL,
0x1154d04f00080ULL,
0x600400140002510ULL,
0x183202a88133ccULL,
0x201701000010000ULL,
0x10080106404108ULL,
0xa0082810001002ULL,
0x1108080040110100ULL,
0x201042202001400ULL,
0x200180280082010ULL,
0x2040080401082102ULL,
0x408200a000021081ULL,
0x500a1000808104ULL,
0x100042088560200ULL,
0x20a200840004208ULL,
0x410a208a0020800ULL,
0x40040848840880ULL,
0x1004022051040310ULL,
0x4510100480810200ULL,
0x201042202001400ULL,
0x8010002488108ULL,
0x402000c1048102ULL,
0x4000a18801140800ULL,
0x410800880800408ULL,
0x84008004040211ULL,
0x2004810040202400ULL,
0x8000200481880820ULL,
0x2004804000810ULL,
0x204110480208042ULL,
0x402080120a0001ULL,
0x200a042208051080ULL,
0x4000201080080080ULL,
0xf800208044008010ULL,
0x2082088204401024ULL,
0x1080402080420210ULL,
0xa00880200820020ULL,
0x100420080080084ULL,
0x400080a020041000ULL,
0x208020248004300ULL,
0x84404402080208ULL,
0x20004088142080ULL,
0x8848a412002000ULL,
0x824820010040101ULL,
0x108020081140100ULL,
0x220800101304020ULL,
0xa00042020120010ULL,
0x1000200300080811ULL
};

}
