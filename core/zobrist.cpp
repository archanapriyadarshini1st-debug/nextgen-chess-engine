#include "zobrist.h"
#include "position.h"
#include <array>
#include <random>

namespace chess::zobrist {
namespace {
std::array<std::array<Key, 64>, 12> pieces{};
Key side_key{};
std::array<Key, 16> castle{};
std::array<Key, 64> ep{};
bool initialized = false;

Key next_key() {
    static std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
    return rng();
}
}

void init() {
    if (initialized) return;
    for (auto& p : pieces) for (auto& x : p) x = next_key();
    for (auto& x : castle) x = next_key();
    for (auto& x : ep) x = next_key();
    side_key = next_key();
    initialized = true;
}

Key compute(const Position& pos) {
    init();
    Key k = 0;
    const auto& b = pos.board();
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b[sq];
        if (p != Piece::None) k ^= pieces[piece_index(p)][sq];
    }
    k ^= castle[pos.castling_rights() & 15u];
    if (pos.ep_square() >= 0) k ^= ep[pos.ep_square()];
    if (pos.side_to_move() == Color::Black) k ^= side_key;
    return k;
}

} // namespace chess::zobrist
