#include "zobrist.h"
#include "position.h"
#include <random>

namespace chess::zobrist {

std::array<std::array<Key, 64>, 12> PieceKeys{};
std::array<Key, 16> CastleKeys{};
std::array<Key, 64> EpKeys{};
Key SideKey{};

namespace {
bool initialized = false;
}

void init() {
    if (initialized) return;
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
    for (auto& p : PieceKeys) for (auto& x : p) x = rng();
    for (auto& x : CastleKeys) x = rng();
    for (auto& x : EpKeys) x = rng();
    SideKey = rng();
    initialized = true;
}

Key compute(const Position& pos) {
    init();
    Key k = 0;
    const auto& b = pos.board();
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b[sq];
        if (p != Piece::None) k ^= PieceKeys[piece_index(p)][sq];
    }
    k ^= CastleKeys[pos.castling_rights() & 15u];
    if (pos.ep_square() >= 0) k ^= EpKeys[pos.ep_square()];
    if (pos.side_to_move() == Color::Black) k ^= SideKey;
    return k;
}

}
