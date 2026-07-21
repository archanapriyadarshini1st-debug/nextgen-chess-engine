
#include "eval.h"
#include <bit>
#include <cmath>

namespace chess {
namespace {
int value(Piece p) {
    switch (p) {
        case Piece::WP: case Piece::BP: return 100;
        case Piece::WN: case Piece::BN: return 320;
        case Piece::WB: case Piece::BB: return 330;
        case Piece::WR: case Piece::BR: return 500;
        case Piece::WQ: case Piece::BQ: return 900;
        default: return 0;
    }
}
int pst(Piece p, int sq) {
    int f = file_of(sq), r = rank_of(sq);
    int df = std::abs(3 - f), dr = std::abs(3 - r);
    int center = 6 - (df + dr);
    switch (p) {
        case Piece::WP: return r * 9 + center;
        case Piece::BP: return (7 - r) * 9 + center;
        case Piece::WN: case Piece::BN: return center * 4 - (std::abs(f - 3) + std::abs(r - 3));
        case Piece::WB: case Piece::BB: return center * 3;
        case Piece::WR: case Piece::BR: return (f == 0 || f == 7) ? 8 : 0;
        case Piece::WQ: case Piece::BQ: return center * 2;
        case Piece::WK: return -r * 6;
        case Piece::BK: return -(7 - r) * 6;
        default: return 0;
    }
}
}

Score evaluate(const Position& pos) {
    Score score = 0;
    const auto& b = pos.board();
    int white_bishops = 0, black_bishops = 0;
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b[sq];
        if (p == Piece::None) continue;
        Score s = value(p) + pst(p, sq);
        if (is_white(p)) score += s;
        else score -= s;
        if (p == Piece::WB) ++white_bishops;
        if (p == Piece::BB) ++black_bishops;
    }
    if (white_bishops >= 2) score += 28;
    if (black_bishops >= 2) score -= 28;
    if (pos.side_to_move() == Color::Black) score = -score;
    return score + 8;
}

} // namespace chess
