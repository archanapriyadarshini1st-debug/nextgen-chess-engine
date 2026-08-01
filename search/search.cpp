#include "search.h"
#include "../core/movegen.h"
#include "../core/bitboard.h"
#include "../evaluation/eval.h"
#include "../tb/syzygy.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace chess {
namespace {

int piece_value(Piece p) {
    switch (p) {
        case Piece::WP: case Piece::BP: return 100;
        case Piece::WN: case Piece::BN: return 320;
        case Piece::WB: case Piece::BB: return 330;
        case Piece::WR: case Piece::BR: return 500;
        case Piece::WQ: case Piece::BQ: return 900;
        default: return 0;
    }
}
int promo_bonus(std::uint32_t promo) {
    switch (promo) { case 1: return 320; case 2: return 330; case 3: return 500; case 4: return 900; default: return 0; }
}
int piece_type_slot(Piece p) { return piece_index(p); }

inline Score value_from_tt(Score v, int ply) {
    if (v >=  MATE_SCORE - 1000) return static_cast<Score>(v - ply);
    if (v <= -MATE_SCORE + 1000) return static_cast<Score>(v + ply);
    return v;
}
inline Score value_to_tt(Score v, int ply) {
    if (v >=  MATE_SCORE - 1000) return static_cast<Score>(v + ply);
    if (v <= -MATE_SCORE + 1000) return static_cast<Score>(v - ply);
    return v;
}

} // anon

// --------------------------------------------------------------------------
// SPSA parameters
// --------------------------------------------------------------------------
const std::vector<std::string>& SearchParams::names() {
    static const std::vector<std::string> n = {
        "LmrBase", "LmrMult", "FutilityBase", "FutilityMult",
        "RfpBase", "RfpMult", "NullRBase", "HistoryBonus", "Aspiration"
    };
    return n;
}
bool SearchParams::set(const std::string& k, int v) {
    if (k == "LmrBase")      { lmr_base = v; return true; }
    if (k == "LmrMult")      { lmr_mult = v; return true; }
    if (k == "FutilityBase") { futility_base = v; return true; }
    if (k == "FutilityMult") { futility_mult = v; return true; }
    if (k == "RfpBase")      { rfp_base = v; return true; }
    if (k == "RfpMult")      { rfp_mult = v; return true; }
    if (k == "NullRBase")    { null_r_base = v; return true; }
    if (k == "HistoryBonus") { history_bonus = v; return true; }
    if (k == "Aspiration")   { aspiration = v; return true; }
    return false;
}
int SearchParams::get(const std::string& k) const {
    if (k == "LmrBase")      return lmr_base;
    if (k == "LmrMult")      return lmr_mult;
    if (k == "FutilityBase") return futility_base;
    if (k == "FutilityMult") return futility_mult;
    if (k == "RfpBase")      return rfp_base;
    if (k == "RfpMult")      return rfp_mult;
    if (k == "NullRBase")    return null_r_base;
    if (k == "HistoryBonus") return history_bonus;
    if (k == "Aspiration")   return aspiration;
    return 0;
}

// --------------------------------------------------------------------------
Search::Search()
    : tt_owned_(std::make_shared<TT>(16)) {
    tt_ = tt_owned_.get();
    stop_ = &stop_owned_;
    init_tables();
}

// Lazy SMP helper: points at the SAME TT as the main thread (no copy!) and
// shares the stop flag, but keeps its own history / killers / counter moves.
Search::Search(TT* shared, std::atomic<bool>* stop)
    : tt_(shared), stop_(stop) {
    init_tables();
}

void Search::clear() {
    tt_->clear();
    for (auto& h : history_) for (auto& v : h) v = 0;
    for (auto& k : killers_) for (auto& m : k) m = Move{};
    for (auto& c : countermove_) for (auto& m : c) m = Move{};
    for (auto& c : capture_history_) for (auto& p : c) for (auto& v : p) v = 0;
    for (auto& c : cont_history_) for (auto& f : c) for (auto& t : f) t = 0;
}

void Search::init_tables() {
    const double base = params_.lmr_base / 100.0;
    const double mult = params_.lmr_mult / 100.0;
    for (int d = 1; d < 64; ++d)
        for (int m = 1; m < 64; ++m)
            lmr_table_[d][m] = static_cast<int>(base + std::log(d) * std::log(m) * mult);
    for (int d = 0; d < 16; ++d) {
        futility_margin_[d] = params_.futility_mult * d + params_.futility_base;
        reverse_futility_margin_[d] = params_.rfp_mult * d + params_.rfp_base;
    }
}

int Search::piece_slot(Piece p) { return piece_type_slot(p); }

bool Search::time_up() const {
    if (stopped()) return true;
    if (limit_ms_ <= 0) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_).count();
    return elapsed >= limit_ms_;
}

int Search::complexity_score(const Position& pos) const {
    MoveList moves;
    Position copy = pos;
    generate_moves(copy, moves, false);
    int material = 0, pawns = 0;
    const auto& b = pos.board();
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b[sq];
        if (p == Piece::None) continue;
        material += piece_value(p);
        if (p == Piece::WP || p == Piece::BP) ++pawns;
    }
    int caps = 0;
    for (int i = 0; i < moves.size; ++i) if (moves.moves[i].flags() & FLAG_CAPTURE) ++caps;
    return (pos.in_check(pos.side_to_move()) ? 40 : 0)
         + std::min(40, moves.size * 2)
         + (material < 2400 ? 25 : 0)
         + (pawns <= 6 ? 20 : 0)
         + std::min(20, caps * 5);
}

int Search::adaptive_depth(const Position& pos, const Limits& limits) const {
    if (limits.depth > 0) return limits.depth;
    const int c = complexity_score(pos);
    int base = c < 35 ? 18 : c < 60 ? 22 : c < 85 ? 26 : c < 110 ? 30 : 34;
    if (limit_ms_ > 0) {
        if (limit_ms_ < 800)        base = std::min(base, 14);
        else if (limit_ms_ < 1500)  base = std::min(base, 18);
        else if (limit_ms_ < 4000)  base = std::min(base, 22);
        else if (limit_ms_ < 10000) base = std::min(base, 26);
        else                        base = std::min(base, 30);
    }
    return std::clamp(base, 12, 40);
}

// --------------------------------------------------------------------------
// Move ordering: score EVERY move exactly once, then a single sort.
// The old code called score_move (with a full SEE) inside an O(n^2) selection
// loop - ~1600 SEE calls per node on a 40 move list.
// --------------------------------------------------------------------------
int Search::score_move(const Position& pos, Move m, Move tt_move, int ply) const {
    if (!tt_move.is_null() && m.raw == tt_move.raw) return 4'000'000;

    const Piece fromP = pos.piece_at(m.from());
    const int slot = piece_slot(fromP);

    if (m.flags() & FLAG_PROMOTION)
        return 3'000'000 + promo_bonus(m.promo()) + ((m.flags() & FLAG_CAPTURE) ? 1000 : 0);

    if (m.flags() & FLAG_CAPTURE) {
        Piece victim = (m.flags() & FLAG_EN_PASSANT)
            ? (pos.side_to_move() == Color::White ? Piece::BP : Piece::WP)
            : pos.piece_at(m.to());
        const int mvvlva = piece_value(victim) * 16 - piece_value(fromP);
        const int ch = (slot >= 0)
            ? capture_history_[static_cast<int>(pos.side_to_move())][slot % 6][m.to()] / 8 : 0;
        const bool good = see_ge(pos, m, -20);          // exactly one SEE per move
        return (good ? 2'000'000 : 100'000) + mvvlva + ch;
    }

    if (killers_[ply][0].raw == m.raw) return 1'900'000;
    if (killers_[ply][1].raw == m.raw) return 1'800'000;

    int s = 0;
    if (slot >= 0) {
        s += history_[slot][m.to()];
        s += cont_history_[slot][m.from()][m.to()] / 2;
    }
    return s;
}

void Search::order_moves(const Position& pos, MoveList& moves, Move tt_move, int ply) const {
    struct Scored { int s; Move m; };
    std::array<Scored, 256> buf;
    const int n = moves.size;
    for (int i = 0; i < n; ++i)
        buf[i] = Scored{ score_move(pos, moves.moves[i], tt_move, ply), moves.moves[i] };
    std::sort(buf.begin(), buf.begin() + n,
              [](const Scored& a, const Scored& b) { return a.s > b.s; });
    for (int i = 0; i < n; ++i) moves.moves[i] = buf[i].m;
}

// --------------------------------------------------------------------------
Score Search::qsearch(Position& pos, Score alpha, Score beta, int ply) {
    seldepth_ = std::max(seldepth_, ply);
    ++nodes_;
    if ((nodes_ & 2047) == 0 && time_up()) return evaluate(pos);
    if (ply >= MAX_PLY - 1) return evaluate(pos);
    if (pos.is_draw(ply)) return 0;

    const TTData tte = tt_->probe(pos.zobrist());
    Move tt_move{};
    if (tte.hit) {
        tt_move = tte.best;
        const Score s = value_from_tt(tte.score, ply);
        if (tte.depth >= 0 && std::abs(s) < MATE_SCORE - 1000) {
            if (tte.flag == TTFlag::Exact) return s;
            if (tte.flag == TTFlag::Beta  && s >= beta)  return s;
            if (tte.flag == TTFlag::Alpha && s <= alpha) return s;
        }
    }

    const bool inCheck = pos.in_check(pos.side_to_move());
    Score stand = -INF;
    if (!inCheck) {
        stand = evaluate(pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }

    MoveList moves;
    generate_moves(pos, moves, !inCheck);
    if (inCheck && moves.size == 0) return static_cast<Score>(-MATE_SCORE + ply);
    order_moves(pos, moves, tt_move, ply);

    Score best = stand;
    for (int i = 0; i < moves.size; ++i) {
        const Move m = moves.moves[i];
        if (!inCheck) {
            const Piece victim = pos.piece_at(m.to());
            if (victim != Piece::None && stand + piece_value(victim) + 200 < alpha) continue; // delta
            if (!see_ge(pos, m, -50)) continue;
        }
        if (!pos.make_move(m)) continue;
        const Score score = static_cast<Score>(-qsearch(pos, static_cast<Score>(-beta), static_cast<Score>(-alpha), ply + 1));
        pos.unmake_move();
        if (score > best) best = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
        if (time_up()) break;
    }
    return best;
}

Score Search::negamax(Position& pos, int depth, Score alpha, Score beta, int ply, bool cutNode) {
    seldepth_ = std::max(seldepth_, ply);
    ++nodes_;
    if ((nodes_ & 1023) == 0 && time_up()) return evaluate(pos);
    if (depth <= 0) return qsearch(pos, alpha, beta, ply);
    if (ply >= MAX_PLY - 1) return evaluate(pos);
    if (ply > 0 && pos.is_draw(ply)) return 0;

    const bool inCheck = pos.in_check(pos.side_to_move());
    if (inCheck) ++depth;

    tt_->prefetch(pos.zobrist());
    const TTData tte = tt_->probe(pos.zobrist());
    Move tt_move{};
    if (tte.hit) {
        tt_move = tte.best;
        const Score s = value_from_tt(tte.score, ply);
        if (ply > 0 && tte.depth >= depth) {
            if (tte.flag == TTFlag::Exact) return s;
            if (tte.flag == TTFlag::Beta  && s >= beta)  return s;
            if (tte.flag == TTFlag::Alpha && s <= alpha) return s;
        }
    }

    const Score staticEval = tte.hit ? tte.eval : evaluate(pos);

    // Syzygy
    if (ply > 0 && SyzygyTablebase::is_ready()) {
        SyzygyTablebase tb;
        if (auto wdl = tb.probe_wdl(pos)) {
            const Score tbScore = (*wdl > 0) ? static_cast<Score>(MATE_SCORE - MAX_PLY - ply)
                                : (*wdl < 0) ? static_cast<Score>(-MATE_SCORE + MAX_PLY + ply)
                                             : Score{0};
            const TTFlag flag = (*wdl == 0) ? TTFlag::Exact : (*wdl > 0 ? TTFlag::Beta : TTFlag::Alpha);
            if (flag == TTFlag::Exact || (flag == TTFlag::Beta && tbScore >= beta)
                                      || (flag == TTFlag::Alpha && tbScore <= alpha)) {
                tt_->store(pos.zobrist(), depth, value_to_tt(tbScore, ply), flag, Move{}, staticEval);
                return tbScore;
            }
        }
    }

    if (!inCheck && depth <= 7 && staticEval - reverse_futility_margin_[depth] >= beta)
        return staticEval;

    if (!inCheck && depth >= 3 && ply > 0 && staticEval >= beta) {
        const Color us = pos.side_to_move();
        const Bitboard nonPawn = pos.pieces(us, 2) | pos.pieces(us, 3) | pos.pieces(us, 4) | pos.pieces(us, 5);
        if (nonPawn) {
            pos.make_null_move();
            int R = params_.null_r_base + depth / 4 + (staticEval - beta) / 200;
            R = std::min(R, depth);
            const Score score = static_cast<Score>(-negamax(pos, depth - R, static_cast<Score>(-beta), static_cast<Score>(-beta + 1), ply + 1, !cutNode));
            pos.unmake_move();
            if (score >= beta) return beta;
        }
    }

    if (tt_move.is_null() && depth >= 4) {
        negamax(pos, depth / 2, alpha, beta, ply, cutNode);
        const TTData e2 = tt_->probe(pos.zobrist());
        if (e2.hit) tt_move = e2.best;
    }

    MoveList moves;
    generate_moves(pos, moves, false);
    if (moves.size == 0) return inCheck ? static_cast<Score>(-MATE_SCORE + ply) : Score{0};
    order_moves(pos, moves, tt_move, ply);

    const Score alpha0 = alpha;
    Move best{};
    Score best_score = -INF;
    int movesTried = 0;

    for (int i = 0; i < moves.size; ++i) {
        const Move m = moves.moves[i];
        const bool isCapture = (m.flags() & FLAG_CAPTURE) != 0;
        const bool isPromo   = (m.flags() & FLAG_PROMOTION) != 0;
        const Piece movedPiece = pos.piece_at(m.from());

        if (!inCheck && !isCapture && !isPromo && depth <= 4 && movesTried > depth * depth + 4) break;
        if (!inCheck && !isCapture && !isPromo && depth <= 4 && movesTried > 0
            && staticEval + futility_margin_[depth] + 80 * movesTried <= alpha) { ++movesTried; continue; }
        if (depth <= 6 && !isPromo && !inCheck && movesTried > 0) {
            const int thresh = isCapture ? -50 * depth : -(depth * depth * 10);
            if (!see_ge(pos, m, thresh)) { ++movesTried; continue; }
        }

        if (!pos.make_move(m)) continue;

        const int new_depth = depth - 1 + (inCheck ? 1 : 0);
        Score score;

        if (movesTried >= 3 && depth >= 3 && !isCapture && !isPromo && !inCheck) {
            int r = lmr_table_[std::min(depth, 63)][std::min(movesTried, 63)];
            if (cutNode) ++r;
            const int slot = piece_slot(movedPiece);
            if (slot >= 0) r -= history_[slot][m.to()] / 8192;
            r = std::max(0, std::min(r, depth - 2));
            score = static_cast<Score>(-negamax(pos, new_depth - r, static_cast<Score>(-alpha - 1), static_cast<Score>(-alpha), ply + 1, true));
            if (score > alpha && r > 0)
                score = static_cast<Score>(-negamax(pos, new_depth, static_cast<Score>(-alpha - 1), static_cast<Score>(-alpha), ply + 1, !cutNode));
            if (score > alpha && score < beta)
                score = static_cast<Score>(-negamax(pos, new_depth, static_cast<Score>(-beta), static_cast<Score>(-alpha), ply + 1, false));
        } else if (movesTried == 0) {
            score = static_cast<Score>(-negamax(pos, new_depth, static_cast<Score>(-beta), static_cast<Score>(-alpha), ply + 1, false));
        } else {
            score = static_cast<Score>(-negamax(pos, new_depth, static_cast<Score>(-alpha - 1), static_cast<Score>(-alpha), ply + 1, true));
            if (score > alpha && score < beta)
                score = static_cast<Score>(-negamax(pos, new_depth, static_cast<Score>(-beta), static_cast<Score>(-alpha), ply + 1, false));
        }

        pos.unmake_move();
        ++movesTried;

        if (time_up() && movesTried > 1) break;

        if (score > best_score) { best_score = score; best = m; }
        if (score > alpha) {
            alpha = score;
            if (score >= beta) {
                const int sl = piece_slot(movedPiece);
                if (!isCapture) {
                    killers_[ply][1] = killers_[ply][0];
                    killers_[ply][0] = m;
                    if (sl >= 0) {
                        const int bonus = params_.history_bonus * depth * depth;
                        history_[sl][m.to()] += bonus;
                        if (history_[sl][m.to()] > 1 << 20) for (int f = 0; f < 64; ++f) history_[sl][f] /= 2;
                        cont_history_[sl][m.from()][m.to()] += bonus;
                    }
                } else if (sl >= 0) {
                    capture_history_[static_cast<int>(pos.side_to_move())][sl % 6][m.to()] += depth * depth;
                }
                break;
            }
        }
    }

    if (movesTried == 0) return inCheck ? static_cast<Score>(-MATE_SCORE + ply) : Score{0};

    const TTFlag flag = (best_score <= alpha0) ? TTFlag::Alpha
                      : (best_score >= beta)   ? TTFlag::Beta
                                               : TTFlag::Exact;
    tt_->store(pos.zobrist(), depth, value_to_tt(best_score, ply), flag, best, staticEval);
    return best_score;
}

std::vector<Move> Search::extract_pv(Position pos, int depth) const {
    std::vector<Move> pv;
    for (int i = 0; i < depth; ++i) {
        const TTData e = tt_->probe(pos.zobrist());
        if (!e.hit || e.best.is_null()) break;
        MoveList legal;
        generate_moves(pos, legal, false);
        bool found = false;
        for (int j = 0; j < legal.size; ++j) if (legal.moves[j].raw == e.best.raw) { found = true; break; }
        if (!found) break;
        if (!pos.make_move(e.best)) break;
        pv.push_back(e.best);
    }
    return pv;
}

// --------------------------------------------------------------------------
// One iterative-deepening loop. Every Lazy SMP thread runs this on its own
// copy of the root position; they only communicate through the shared TT.
// --------------------------------------------------------------------------
SearchResult Search::id_loop(Position pos, int max_depth, unsigned thread_id) {
    SearchResult r;
    MoveList root;
    generate_moves(pos, root, false);
    if (root.size == 0) {
        r.score = pos.in_check(pos.side_to_move()) ? -MATE_SCORE : 0;
        return r;
    }
    r.best_move = root.moves[0];

    Score previous = 0;
    for (int depth = 1; depth <= max_depth; ++depth) {
        if (time_up()) break;
        // Lazy SMP: helpers stagger their depths so they explore different trees.
        if (thread_id > 0 && depth > 3 && ((depth + static_cast<int>(thread_id)) % 3) == 0) continue;

        Score window = (depth >= 4) ? static_cast<Score>(params_.aspiration) : static_cast<Score>(INF);
        Move iter_best{};
        Score iter_score = 0;
        bool accepted = false;

        while (!accepted) {
            Score alpha = (depth >= 4) ? static_cast<Score>(previous - window) : static_cast<Score>(-INF);
            Score beta  = (depth >= 4) ? static_cast<Score>(previous + window) : static_cast<Score>(INF);
            const Score alpha0 = alpha, beta0 = beta;

            const TTData e = tt_->probe(pos.zobrist());
            order_moves(pos, root, e.hit ? e.best : Move{}, 0);

            Score local_best = -INF;
            Move local_move = root.moves[0];
            for (int i = 0; i < root.size; ++i) {
                const Move m = root.moves[i];
                if (!pos.make_move(m)) continue;
                Score score;
                if (i == 0) score = static_cast<Score>(-negamax(pos, depth - 1, static_cast<Score>(-beta), static_cast<Score>(-alpha), 1, false));
                else {
                    score = static_cast<Score>(-negamax(pos, depth - 1, static_cast<Score>(-alpha - 1), static_cast<Score>(-alpha), 1, true));
                    if (score > alpha && score < beta)
                        score = static_cast<Score>(-negamax(pos, depth - 1, static_cast<Score>(-beta), static_cast<Score>(-alpha), 1, false));
                }
                pos.unmake_move();
                if (score > local_best) { local_best = score; local_move = m; }
                if (score > alpha) alpha = score;
                if (alpha >= beta) break;
                if (time_up()) break;
            }

            if (time_up()) { accepted = true; if (local_best > -INF) { iter_best = local_move; iter_score = local_best; } break; }
            if (depth >= 4 && (local_best <= alpha0 || local_best >= beta0)) {
                previous = local_best;
                window = static_cast<Score>(std::min<int>(INF, window * 3));
                continue;
            }
            iter_best = local_move;
            iter_score = local_best;
            previous = local_best;
            accepted = true;
        }

        if (!iter_best.is_null()) {
            r.best_move = iter_best;
            r.score = iter_score;
            r.depth = depth;
        }
        if (time_up()) break;
    }
    r.nodes = nodes_;
    r.seldepth = seldepth_;
    return r;
}

SearchResult Search::think(Position& pos, const Limits& limits) {
    nodes_ = 0;
    seldepth_ = 0;
    start_ = std::chrono::steady_clock::now();
    stop_->store(false, std::memory_order_relaxed);
    tt_->new_search();

    if (limits.movetime_ms > 0) limit_ms_ = limits.movetime_ms;
    else if (limits.infinite) limit_ms_ = 0;
    else {
        const auto remaining = (pos.side_to_move() == Color::White) ? limits.wtime_ms : limits.btime_ms;
        const auto inc = (pos.side_to_move() == Color::White) ? limits.winc_ms : limits.binc_ms;
        limit_ms_ = remaining > 0
            ? std::max<std::int64_t>(10, remaining / std::max(1, limits.movestogo ? limits.movestogo : 30) + inc * 7 / 10)
            : 0;
    }

    const int max_depth = adaptive_depth(pos, limits);

    // ---- true Lazy SMP: N threads, ONE shared TT, per-thread history ----
    std::vector<std::unique_ptr<Search>> helpers;
    std::vector<std::thread> pool;
    const unsigned n = std::max(1u, threads_);
    for (unsigned t = 1; t < n; ++t) {
        auto h = std::make_unique<Search>(tt_, stop_);   // shares the table, no copy
        h->params_ = params_;
        h->init_tables();
        h->start_ = start_;
        h->limit_ms_ = limit_ms_;
        h->chess960_ = chess960_;
        helpers.push_back(std::move(h));
    }
    for (unsigned t = 1; t < n; ++t) {
        Search* h = helpers[t - 1].get();
        Position copy = pos;
        pool.emplace_back([h, copy, max_depth, t]() mutable { h->id_loop(copy, max_depth, t); });
    }

    SearchResult r = id_loop(pos, max_depth, 0);

    stop_->store(true, std::memory_order_relaxed);
    for (auto& th : pool) th.join();
    for (auto& h : helpers) r.nodes += h->nodes_;
    stop_->store(false, std::memory_order_relaxed);

    r.seldepth = std::max(r.seldepth, r.depth);
    r.pv.clear();
    if (!r.best_move.is_null()) {
        r.pv.push_back(r.best_move);
        Position child = pos;
        if (child.make_move(r.best_move)) {
            auto child_pv = extract_pv(child, std::max(0, r.depth - 1));
            r.pv.insert(r.pv.end(), child_pv.begin(), child_pv.end());
        }
    }
    return r;
}

} // namespace chess
