#pragma once
#include "types.h"
#include <immintrin.h>

namespace Bitboard {

// `attacks` points at this square's slice of the shared table.
// Indexing is always relative to `attacks`, never to a global offset.
struct Magic {
  uint64_t mask;
  uint64_t magic;
  unsigned shift;
  uint64_t* attacks;

  // Index of `occ` within this square's slice.
  inline unsigned index(uint64_t occ) const {
#if defined(USE_PEXT)
    return static_cast<unsigned>(_pext_u64(occ, mask));
#else
    return static_cast<unsigned>(((occ & mask) * magic) >> shift);
#endif
  }
};

extern uint64_t rook_table[102400];
extern uint64_t bishop_table[5248];
extern Magic rook_magics[64];
extern Magic bishop_magics[64];

extern uint64_t knight_attacks[64];
extern uint64_t king_attacks[64];
extern uint64_t pawn_attacks[2][64];

void init();

// Brute-force check of every square against every relevant occupancy.
// Called by init(); exposed so tests can assert it independently.
bool verify();

inline uint64_t rook_attacks(int sq, uint64_t occ) {
  const Magic& m = rook_magics[sq];
  return m.attacks[m.index(occ)];
}

inline uint64_t bishop_attacks(int sq, uint64_t occ) {
  const Magic& m = bishop_magics[sq];
  return m.attacks[m.index(occ)];
}

inline uint64_t queen_attacks(int sq, uint64_t occ) {
  return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}

inline int popcount(uint64_t b){ return __builtin_popcountll(b); }
inline int lsb_index(uint64_t b){ return __builtin_ctzll(b); }
inline void prefetch(void* p){ _mm_prefetch((const char*)p, _MM_HINT_T0); }

}
