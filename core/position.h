#pragma once

#include "types.h"
#include <array>
#include <vector>

namespace chess {

struct Undo {
    Move move{};
    Piece captured{Piece::None};
    std::uint8_t castling_rights{0};
    std::int8_t ep_square{-1};
    std::uint16_t halfmove_clock{0};
    Key key{0};
    Score eval_cache{0};
};

class Position {
public:
    void set_startpos();
    void set_fen(const std::string& fen);
    std::string fen() const;

    bool make_move(Move m);
    void unmake_move();
    bool legal(Move m) const;
    bool in_check(Color side) const;

    Color side_to_move() const { return stm_; }
    Key zobrist() const { return key_; }
    const std::array<Bitboard, 12>& piece_bb() const { return piece_bb_; }

private:
    std::array<Bitboard, 12> piece_bb_{};
    std::array<Bitboard, 2> occ_{};
    std::array<Piece, 64> board_{};
    Color stm_{Color::White};
    std::uint8_t castling_rights_{0};
    std::int8_t ep_square_{-1};
    std::uint16_t halfmove_clock_{0};
    std::uint16_t fullmove_number_{1};
    Key key_{0};
    Score eval_cache_{0};
    std::vector<Undo> history_;
};

} // namespace chess
