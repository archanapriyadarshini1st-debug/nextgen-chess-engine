#pragma once
#include "types.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace Bitboard {

struct Magic {
  uint64_t  mask;
  uint64_t  magic;
  unsigned  shift;
  uint64_t* attacks;   // points into rook_table / bishop_table
};

extern uint64_t rook_table[102400];
extern uint64_t bishop_table[5248];
extern Magic rook_magics[64];
extern Magic bishop_magics[64];

extern uint64_t knight_attacks[64];
extern uint64_t king_attacks[64];
extern uint64_t pawn_attacks[2][64];
extern uint64_t between_bb[64][64];
extern uint64_t line_bb[64][64];

void init();

// NOTE: the attack tables are built so that the index is produced by the SAME
// scheme the lookup uses (PEXT when BMI2 is available, magic multiply otherwise).
// The old code indexed rook_table[shift + idx] which mixed a shift amount with a
// table offset - that was the "PEXT fallback" bug.
inline uint64_t rook_attacks(int sq, uint64_t occ) {
  const Magic& m = rook_magics[sq];
#if defined(USE_PEXT) && defined(__BMI2__)
  return m.attacks[_pext_u64(occ, m.mask)];
#else
  return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
#endif
}

inline uint64_t bishop_attacks(int sq, uint64_t occ) {
  const Magic& m = bishop_magics[sq];
#if defined(USE_PEXT) && defined(__BMI2__)
  return m.attacks[_pext_u64(occ, m.mask)];
#else
  return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
#endif
}

inline uint64_t queen_attacks(int sq, uint64_t occ) {
  return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}

inline int  popcount(uint64_t b) { return __builtin_popcountll(b); }
inline int  lsb_index(uint64_t b){ return __builtin_ctzll(b); }
inline int  pop_lsb(uint64_t& b) { int s = __builtin_ctzll(b); b &= b - 1; return s; }
inline void prefetch(void* p) {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_prefetch((const char*)p, _MM_HINT_T0);
#else
  (void)p;
#endif
}

}
