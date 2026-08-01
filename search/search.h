#pragma once
#include "../core/position.h"
#include "tt.h"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
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

// SPSA-tunable search parameters. Exposed over UCI as spin options so the
// tuner can drive a real engine binary instead of a simulated win rate.
struct SearchParams {
    int lmr_base       = 75;    // x100
    int lmr_mult       = 35;    // x100
    int futility_base  = 80;
    int futility_mult  = 150;
    int rfp_base       = 60;
    int rfp_mult       = 120;
    int null_r_base    = 3;
    int history_bonus  = 16;
    int aspiration     = 50;

    bool set(const std::string& name, int value);
    int  get(const std::string& name) const;
    static const std::vector<std::string>& names();
};

class Search {
public:
    Search();                                       // owns its own TT
    Search(TT* shared, std::atomic<bool>* stop);    // Lazy SMP helper: shares the TT

    void set_hash_mb(std::size_t mb) { tt_->resize_mb(mb); }
    void set_threads(unsigned n) { threads_ = n ? n : 1; }
    unsigned threads() const { return threads_; }
    void set_chess960(bool b) { chess960_ = b; }
    void set_params(const SearchParams& p) { params_ = p; init_tables(); }
    SearchParams& params() { return params_; }
    TT& tt() { return *tt_; }

    void clear();
    SearchResult think(Position& pos, const Limits& limits);

private:
    Score negamax(Position& pos, int depth, Score alpha, Score beta, int ply, bool cutNode);
    Score qsearch(Position& pos, Score alpha, Score beta, int ply);
    void  order_moves(const Position& pos, MoveList& moves, Move tt_move, int ply) const;
    int   score_move(const Position& pos, Move m, Move tt_move, int ply) const;
    std::vector<Move> extract_pv(Position pos, int depth) const;
    bool  time_up() const;
    bool  stopped() const { return stop_->load(std::memory_order_relaxed); }
    static int piece_slot(Piece p);
    int   complexity_score(const Position& pos) const;
    int   adaptive_depth(const Position& pos, const Limits& limits) const;
    void  init_tables();
    SearchResult id_loop(Position pos, int max_depth, unsigned thread_id);

    // ---- shared across Lazy SMP threads ----
    std::shared_ptr<TT> tt_owned_;      // only the main Search owns storage
    TT* tt_{nullptr};                   // every thread points at the SAME table
    std::atomic<bool> stop_owned_{false};
    std::atomic<bool>* stop_{nullptr};

    // ---- strictly per-thread ----
    SearchParams params_{};
    std::array<std::array<int, 64>, 12> history_{};
    std::array<std::array<Move, 2>, MAX_PLY> killers_{};
    std::array<std::array<std::array<int,64>, 6>, 2> capture_history_{};
    std::array<std::array<std::array<int,64>,64>,12> cont_history_{};
    std::array<std::array<Move, 64>, 12> countermove_{};

    std::chrono::steady_clock::time_point start_;
    std::int64_t limit_ms_{0};
    std::uint64_t nodes_{0};
    unsigned threads_{1};
    int seldepth_{0};
    int lmr_table_[64][64]{};
    int futility_margin_[16]{};
    int reverse_futility_margin_[16]{};
    bool chess960_{false};
};

} // namespace chess
