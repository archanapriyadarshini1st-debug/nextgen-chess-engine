#pragma once
#include "types.h"
#include <array>
#include <cstdint>

namespace chess { class Position; }

namespace chess::zobrist {

extern std::array<std::array<Key, 64>, 12> PieceKeys;
extern std::array<Key, 16> CastleKeys;
extern std::array<Key, 64> EpKeys;
extern Key SideKey;

void init();
Key compute(const Position& pos);   // full rebuild - debug / set_fen only

inline Key piece_key(int pieceIdx, int sq) { return PieceKeys[pieceIdx][sq]; }
inline Key castle_key(int rights)          { return CastleKeys[rights & 15]; }
inline Key ep_key(int sq)                  { return EpKeys[sq]; }
inline Key side_key()                      { return SideKey; }

}
