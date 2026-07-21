#pragma once
#include "../core/position.h"
#include "tt.h"
#include <array>
#include <chrono>
#include <vector>

namespace chess {

struct SearchResult {
    Move best_move{};
    Score score{0};
    std::vector<Move> pv;
    int depth{0};
    int seldepth{0};
    std::uint64_t nodes{0};
};

class Search {
public:
    Search() { init_tables(); }
    void set_hash_mb(std::size_t mb) { tt_.resize_mb(mb); }
    void set_threads(unsigned n) { threads_ = n ? n : 1; }
    unsigned threads() const { return threads_; }
    void clear() { 
        tt_.clear(); 
        for(auto& h: history_) for(auto& v: h) v=0; 
        for(auto& k: killers_) for(auto& m: k) m=Move{}; 
        for(auto& b: butterfly_) for(auto& f: b) for(auto& t: f) t=0;
        for(auto& c: countermove_) for(auto& m: c) m=Move{};
    }
    SearchResult think(Position& pos, const Limits& limits);
    void set_chess960(bool b) { chess960_=b; }

private:
    Score negamax(Position& pos, int depth, Score alpha, Score beta, int ply, bool cutNode);
    Score qsearch(Position& pos, Score alpha, Score beta, int ply);
    void order_moves(Position& pos, MoveList& moves, Move tt_move, int ply) const;
    int score_move(const Position& pos, Move m, Move tt_move, int ply) const;
    std::vector<Move> extract_pv(Position pos, int depth) const;
    bool time_up() const;
    static int piece_slot(Piece p);
    int complexity_score(const Position& pos) const;
    int adaptive_depth(const Position& pos, const Limits& limits) const;
    void init_tables();

    TT tt_;
    // History tables
    std::array<std::array<int, 64>, 12> history_{}; // piece-to
    std::array<std::array<Move, 2>, MAX_PLY> killers_{};
    std::array<std::array<std::array<int,64>, 12>, 2> capture_history_{};
    std::array<std::array<std::array<int,64>,64>,12> cont_history_{};
    std::array<std::array<std::array<int,64>,64>,2> butterfly_{}; // [color][from][to] - Butterfly
    std::array<std::array<Move, 64>, 12> countermove_{}; // [piece][to] -> counter move
    // For recapture extension tracking
    Move prev_capture_move_{};
    int prev_capture_square_{-1};

    std::chrono::steady_clock::time_point start_;
    std::int64_t limit_ms_{0};
    std::uint64_t nodes_{0};
    unsigned threads_{1};
    int seldepth_{0};
    int lmr_table_[64][64]{};
    int futility_margin_[16]{};
    int reverse_futility_margin_[16]{};
    int probcut_margin_[16]{};
    bool chess960_{false};
};

} // namespace chess
