
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
    Search() = default;
    void set_hash_mb(std::size_t mb) { tt_.resize_mb(mb); }
    void set_threads(unsigned n) { threads_ = n ? n : 1; }
    unsigned threads() const { return threads_; }
    SearchResult think(Position& pos, const Limits& limits);

private:
    Score negamax(Position& pos, int depth, Score alpha, Score beta, int ply);
    Score qsearch(Position& pos, Score alpha, Score beta, int ply);
    void order_moves(Position& pos, MoveList& moves, Move tt_move, int ply) const;
    int score_move(const Position& pos, Move m, Move tt_move, int ply) const;
    std::vector<Move> extract_pv(Position pos, int depth) const;
    bool time_up() const;
    static int piece_slot(Piece p);
    int complexity_score(const Position& pos) const;
    int adaptive_depth(const Position& pos, const Limits& limits) const;

    TT tt_;
    std::array<std::array<int, 64>, 12> history_{};
    std::array<std::array<Move, 2>, MAX_PLY> killers_{};
    std::chrono::steady_clock::time_point start_;
    std::int64_t limit_ms_{0};
    std::uint64_t nodes_{0};
    unsigned threads_{1};
    int seldepth_{0};
};

} // namespace chess
