#pragma once
#include "types.h"
#include <immintrin.h>

namespace Bitboard {

struct Magic {
  uint64_t mask;
  uint64_t magic;
  int shift;
  uint64_t* attacks;
};

extern uint64_t rook_table[102400];
extern uint64_t bishop_table[5248];
extern Magic rook_magics[64];
extern Magic bishop_magics[64];

extern uint64_t knight_attacks[64];
extern uint64_t king_attacks[64];
extern uint64_t pawn_attacks[2][64];

void init();

inline uint64_t rook_attacks(int sq, uint64_t occ) {
#if defined(USE_PEXT) && defined(__BMI2__)
  uint64_t idx = _pext_u64(occ, rook_magics[sq].mask);
  return rook_table[rook_magics[sq].shift + idx];
#else
  Magic &m = rook_magics[sq];
  uint64_t idx = ((occ & m.mask) * m.magic) >> m.shift;
  return m.attacks[idx];
#endif
}

inline uint64_t bishop_attacks(int sq, uint64_t occ) {
#if defined(USE_PEXT) && defined(__BMI2__)
  uint64_t idx = _pext_u64(occ, bishop_magics[sq].mask);
  return bishop_table[bishop_magics[sq].shift + idx];
#else
  Magic &m = bishop_magics[sq];
  uint64_t idx = ((occ & m.mask) * m.magic) >> m.shift;
  return m.attacks[idx];
#endif
}

inline uint64_t queen_attacks(int sq, uint64_t occ) {
  return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}

inline int popcount(uint64_t b){ return __builtin_popcountll(b); }
inline int lsb_index(uint64_t b){ return __builtin_ctzll(b); }
inline void prefetch(void* p){ _mm_prefetch((const char*)p, _MM_HINT_T0); }

}
