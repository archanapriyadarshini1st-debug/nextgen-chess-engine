#pragma once

#include <cstdint>
#include <string>

namespace chess {

using Bitboard = std::uint64_t;
using Key = std::uint64_t;
using Score = std::int32_t;

constexpr int kBoardSize = 64;

enum class Color : std::uint8_t { White = 0, Black = 1 };

enum class Piece : std::uint8_t {
    None = 0,
    WP, WN, WB, WR, WQ, WK,
    BP, BN, BB, BR, BQ, BK
};

struct Move {
    std::uint16_t raw{0};
    constexpr Move() = default;
    constexpr explicit Move(std::uint16_t value) : raw(value) {}
    constexpr int from() const { return raw & 63; }
    constexpr int to() const { return (raw >> 6) & 63; }
    constexpr int promo() const { return (raw >> 12) & 7; }
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

} // namespace chess
