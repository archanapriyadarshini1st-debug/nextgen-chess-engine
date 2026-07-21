
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {

using Bitboard = std::uint64_t;
using Key = std::uint64_t;
using Score = std::int32_t;

constexpr Score INF = 30000;
constexpr Score MATE_SCORE = 32000;
constexpr int MAX_PLY = 128;

constexpr std::uint32_t FLAG_CAPTURE = 1u << 0;
constexpr std::uint32_t FLAG_DOUBLE_PUSH = 1u << 1;
constexpr std::uint32_t FLAG_KING_CASTLE = 1u << 2;
constexpr std::uint32_t FLAG_QUEEN_CASTLE = 1u << 3;
constexpr std::uint32_t FLAG_EN_PASSANT = 1u << 4;
constexpr std::uint32_t FLAG_PROMOTION = 1u << 5;

constexpr int WHITE_KINGSIDE = 1;
constexpr int WHITE_QUEENSIDE = 2;
constexpr int BLACK_KINGSIDE = 4;
constexpr int BLACK_QUEENSIDE = 8;

constexpr int file_of(int sq) { return sq & 7; }
constexpr int rank_of(int sq) { return sq >> 3; }
constexpr int square_of(int file, int rank) { return rank * 8 + file; }
constexpr int mirror_square(int sq) { return sq ^ 56; }

enum class Color : std::uint8_t { White = 0, Black = 1 };
constexpr Color opposite(Color c) { return c == Color::White ? Color::Black : Color::White; }

enum class Piece : std::uint8_t {
    None = 0,
    WP, WN, WB, WR, WQ, WK,
    BP, BN, BB, BR, BQ, BK
};

constexpr bool is_white(Piece p) { return p >= Piece::WP && p <= Piece::WK; }
constexpr bool is_black(Piece p) { return p >= Piece::BP && p <= Piece::BK; }
constexpr Color piece_color(Piece p) { return is_white(p) ? Color::White : Color::Black; }

inline int piece_index(Piece p) {
    switch (p) {
        case Piece::WP: return 0; case Piece::WN: return 1; case Piece::WB: return 2;
        case Piece::WR: return 3; case Piece::WQ: return 4; case Piece::WK: return 5;
        case Piece::BP: return 6; case Piece::BN: return 7; case Piece::BB: return 8;
        case Piece::BR: return 9; case Piece::BQ: return 10; case Piece::BK: return 11;
        default: return -1;
    }
}

inline Piece make_piece(Color c, int type) {
    return static_cast<Piece>((c == Color::White ? 0 : 6) + type);
}

struct Move {
    std::uint32_t raw{0};
    constexpr Move() = default;
    constexpr explicit Move(std::uint32_t v) : raw(v) {}
    static constexpr Move make(int from, int to, std::uint32_t flags = 0, std::uint32_t promo = 0) {
        return Move((from & 63u) | ((to & 63u) << 6) | ((promo & 7u) << 12) | ((flags & 0x1FFFFu) << 15));
    }
    constexpr int from() const { return raw & 63u; }
    constexpr int to() const { return (raw >> 6) & 63u; }
    constexpr int promo() const { return (raw >> 12) & 7u; }
    constexpr std::uint32_t flags() const { return raw >> 15; }
    constexpr bool is_null() const { return raw == 0; }
};

struct Limits {
    int depth = 0;
    std::int64_t movetime_ms = 0;
    std::int64_t wtime_ms = 0;
    std::int64_t btime_ms = 0;
    std::int64_t winc_ms = 0;
    std::int64_t binc_ms = 0;
    int movestogo = 0;
    bool infinite = false;
};

struct MoveList {
    std::array<Move, 256> moves{};
    int size{0};
    void push(Move m) { if (size < static_cast<int>(moves.size())) moves[size++] = m; }
    void clear() { size = 0; }
};

} // namespace chess
