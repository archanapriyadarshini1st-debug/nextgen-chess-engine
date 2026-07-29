#pragma once
#include "types.h"
#include <array>
#include <utility>
#include <string>
#include <vector>

namespace chess {

struct Undo {
    Move move{};
    Piece captured{Piece::None};
    int8_t capture_square{-1};
    std::uint8_t castling_rights{0};
    std::int8_t ep_square{-1};
    std::uint16_t halfmove_clock{0};
    std::uint16_t fullmove_number{1};
    Color stm{Color::White};
    Key key{0};
    Score eval_cache{0};
};

class Position {
public:
    void set_startpos();
    void set_fen(const std::string& fen);
    std::string fen() const;

    bool make_move(Move m);
    bool make_null_move();
    void unmake_move();
    bool legal(Move m) const;
    bool in_check(Color side) const;
    bool square_attacked(int sq, Color by) const;
    int king_square(Color side) const;

    // draw detection - Phase 1 absolute correctness
    bool is_draw(int ply) const;
    bool is_threefold() const;
    bool is_fifty_move() const { return halfmove_clock_ >= 100; }
    bool is_seventy_five_move() const { return halfmove_clock_ >= 150; }
    bool is_insufficient_material() const;
    bool is_stalemate() const;
    bool is_checkmate() const;

    Color side_to_move() const { return stm_; }
    Key zobrist() const { return key_; }
    const std::array<Piece, 64>& board() const { return board_; }
    const std::array<Bitboard, 12>& piece_bb() const { return piece_bb_; }
    Bitboard occupancy(Color c) const { return occ_[static_cast<int>(c)]; }
    Bitboard occupancy_all() const { return occ_[0] | occ_[1]; }
    Piece piece_at(int sq) const { return board_[sq]; }
    std::uint8_t castling_rights() const { return castling_rights_; }
    int ep_square() const { return ep_square_; }
    std::uint16_t halfmove_clock() const { return halfmove_clock_; }
    std::uint16_t fullmove_number() const { return fullmove_number_; }
    int game_ply() const { return (fullmove_number_-1)*2 + (stm_==Color::Black?1:0); }
    const std::vector<Key>& key_history() const { return key_history_; }

    // evaluation helpers
    Bitboard pieces(Piece p) const { return piece_bb_[piece_index(p)]; }
    int count(Piece p) const { return __builtin_popcountll(pieces(p)); }

private:
    void clear();
    void put_piece(int sq, Piece p);
    void remove_piece(int sq);
    void refresh_key();

    std::array<Bitboard, 12> piece_bb_{};
    std::array<int8_t, 2> king_squares_{{-1, -1}};
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
    std::vector<Key> key_history_; // for threefold detection
    std::vector<int> halfmove_history_;
};

} // namespace chess
