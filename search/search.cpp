
#include "search.h"
#include "../core/movegen.h"
#include "../evaluation/eval.h"
#include <algorithm>
#include <chrono>
#include <future>
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
    switch (promo) {
        case 1: return 320;
        case 2: return 330;
        case 3: return 500;
        case 4: return 900;
        default: return 0;
    }
}
int piece_type_slot(Piece p) {
    switch (p) {
        case Piece::WP: return 0; case Piece::WN: return 1; case Piece::WB: return 2; case Piece::WR: return 3; case Piece::WQ: return 4; case Piece::WK: return 5;
        case Piece::BP: return 6; case Piece::BN: return 7; case Piece::BB: return 8; case Piece::BR: return 9; case Piece::BQ: return 10; case Piece::BK: return 11;
        default: return -1;
    }
}
}

int Search::piece_slot(Piece p) { return piece_type_slot(p); }

bool Search::time_up() const {
    if (limit_ms_ <= 0) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    return elapsed >= limit_ms_;
}

int Search::score_move(const Position& pos, Move m, Move tt_move, int ply) const {
    if (m.raw == tt_move.raw) return 1'000'000;
    const auto& b = pos.board();
    Piece from = b[m.from()];
    Piece to = b[m.to()];
    int s = 0;
    if (m.flags() & FLAG_CAPTURE) {
        Piece victim = to;
        if ((m.flags() & FLAG_EN_PASSANT)) victim = pos.side_to_move() == Color::White ? Piece::BP : Piece::WP;
        s += 100000 + piece_value(victim) - piece_value(from);
    } else {
        int slot = piece_slot(from);
        if (slot >= 0) s += history_[slot][m.to()];
        if (killers_[ply][0].raw == m.raw) s += 90000;
        else if (killers_[ply][1].raw == m.raw) s += 85000;
    }
    if (m.flags() & FLAG_PROMOTION) s += 80000 + promo_bonus(m.promo());
    return s;
}

void Search::order_moves(Position& pos, MoveList& moves, Move tt_move, int ply) const {
    for (int i = 0; i < moves.size - 1; ++i) {
        int best = i;
        int best_score = score_move(pos, moves.moves[i], tt_move, ply);
        for (int j = i + 1; j < moves.size; ++j) {
            int sc = score_move(pos, moves.moves[j], tt_move, ply);
            if (sc > best_score) { best = j; best_score = sc; }
        }
        if (best != i) std::swap(moves.moves[i], moves.moves[best]);
    }
}

Score Search::qsearch(Position& pos, Score alpha, Score beta, int ply) {
    ++nodes_;
    if (time_up()) return evaluate(pos);
    if (ply >= MAX_PLY - 1) return evaluate(pos);

    Score stand = evaluate(pos);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    MoveList moves;
    if (pos.in_check(pos.side_to_move())) generate_moves(pos, moves, false);
    else generate_moves(pos, moves, true);

    Move tt_move{};
    if (auto* tt = tt_.probe(pos.zobrist())) tt_move = tt->best;
    order_moves(pos, moves, tt_move, ply);

    for (int i = 0; i < moves.size; ++i) {
        Move m = moves.moves[i];
        if (!pos.make_move(m)) continue;
        if (pos.in_check(opposite(pos.side_to_move()))) { pos.unmake_move(); continue; }
        Score score = -qsearch(pos, -beta, -alpha, ply + 1);
        pos.unmake_move();
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
        if (time_up()) break;
    }
    return alpha;
}

Score Search::negamax(Position& pos, int depth, Score alpha, Score beta, int ply) {
    ++nodes_;
    if (time_up()) return evaluate(pos);
    if (depth <= 0) return qsearch(pos, alpha, beta, ply);
    if (ply >= MAX_PLY - 1) return evaluate(pos);

    bool in_check = pos.in_check(pos.side_to_move());
    if (in_check) ++depth;

    if (!in_check && depth <= 2) {
        Score stand = evaluate(pos);
        if (stand + 120 * depth <= alpha) return stand;
    }

    if (!in_check && depth >= 3 && ply > 0) {
        pos.make_null_move();
        Score score = -negamax(pos, depth - 3, -beta, -beta + 1, ply + 1);
        pos.unmake_move();
        if (score >= beta) return beta;
    }

    MoveList moves;
    generate_moves(pos, moves, false);
    if (moves.size == 0) return in_check ? -MATE_SCORE + ply : 0;

    Move tt_move{};
    if (auto* tt = tt_.probe(pos.zobrist())) tt_move = tt->best;
    order_moves(pos, moves, tt_move, ply);

    Score alpha0 = alpha;
    Score beta0 = beta;
    Move best{};
    Score best_score = -INF;

    for (int i = 0; i < moves.size; ++i) {
        Move m = moves.moves[i];
        if (!pos.make_move(m)) continue;

        int new_depth = depth - 1;
        bool quiet = !(m.flags() & FLAG_CAPTURE) && !(m.flags() & FLAG_PROMOTION);
        if (quiet && i > 3 && depth > 2 && !pos.in_check(pos.side_to_move())) new_depth -= 1;
        if (new_depth < 0) new_depth = 0;

        Score score;
        if (i == 0) score = -negamax(pos, new_depth, -beta, -alpha, ply + 1);
        else {
            score = -negamax(pos, new_depth, -alpha - 1, -alpha, ply + 1);
            if (score > alpha && score < beta) score = -negamax(pos, new_depth, -beta, -alpha, ply + 1);
        }

        pos.unmake_move();

        if (score > best_score) { best_score = score; best = m; }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            if (quiet) {
                killers_[ply][1] = killers_[ply][0];
                killers_[ply][0] = m;
                Piece moved = pos.board()[m.from()];
                int slot = piece_slot(moved);
                if (slot >= 0) history_[slot][m.to()] += depth * depth;
            }
            break;
        }
        if (time_up()) break;
    }

    TTFlag flag = TTFlag::Exact;
    if (best_score <= alpha0) flag = TTFlag::Alpha;
    else if (best_score >= beta0) flag = TTFlag::Beta;
    tt_.store(pos.zobrist(), depth, best_score, flag, best);
    return best_score;
}

std::vector<Move> Search::extract_pv(Position pos, int depth) const {
    std::vector<Move> pv;
    for (int i = 0; i < depth; ++i) {
        auto* e = tt_.probe(pos.zobrist());
        if (!e || e->best.is_null()) break;
        Move m = e->best;
        if (!pos.make_move(m)) break;
        pv.push_back(m);
    }
    return pv;
}

SearchResult Search::think(Position& pos, const Limits& limits) {
    SearchResult r;
    nodes_ = 0;
    start_ = std::chrono::steady_clock::now();
    if (limits.movetime_ms > 0) limit_ms_ = limits.movetime_ms;
    else if (limits.infinite) limit_ms_ = 0;
    else {
        auto remaining = pos.side_to_move() == Color::White ? limits.wtime_ms : limits.btime_ms;
        auto inc = pos.side_to_move() == Color::White ? limits.winc_ms : limits.binc_ms;
        if (remaining > 0) limit_ms_ = std::max<std::int64_t>(10, remaining / std::max(1, limits.movestogo ? limits.movestogo : 30) + inc * 7 / 10);
        else limit_ms_ = 0;
    }

    int max_depth = limits.depth > 0 ? limits.depth : 6;
    MoveList root;
    generate_moves(pos, root, false);
    if (root.size == 0) {
        r.best_move = Move{};
        r.score = pos.in_check(pos.side_to_move()) ? -MATE_SCORE : 0;
        r.nodes = nodes_;
        return r;
    }

    Score previous = 0;
    Move best{};
    Score best_score = 0;
    unsigned parallel_threads = std::max(1u, threads_);

    for (int depth = 1; depth <= max_depth; ++depth) {
        if (time_up()) break;
        bool accepted = false;
        std::int64_t window = depth >= 4 ? 50 : INF;
        while (!accepted) {
            Score alpha0 = depth >= 4 ? previous - window : -INF;
            Score beta0 = depth >= 4 ? previous + window : INF;
            Score alpha = alpha0, beta = beta0;

            Move tt_move{};
            if (auto* tt = tt_.probe(pos.zobrist())) tt_move = tt->best;
            order_moves(pos, root, tt_move, 0);

            if (parallel_threads > 1 && depth >= 4 && root.size >= 8) {
                struct RootEval { Move move; Score score; };
                std::vector<std::future<RootEval>> tasks;
                tasks.reserve(root.size);
                const int launch_depth = depth - 1;
                for (int i = 0; i < root.size; ++i) {
                    Move m = root.moves[i];
                    tasks.emplace_back(std::async(std::launch::async, [this, pos, m, launch_depth]() mutable {
                        Search local = *this;
                        Position p = pos;
                        if (!p.make_move(m)) return RootEval{m, -INF};
                        Score sc = -local.negamax(p, launch_depth, -INF, INF, 1);
                        return RootEval{m, sc};
                    }));
                }
                Score local_best = -INF;
                Move local_move{};
                for (auto& fut : tasks) {
                    auto ev = fut.get();
                    if (ev.score > local_best) { local_best = ev.score; local_move = ev.move; }
                }
                best = local_move;
                best_score = local_best;
                previous = local_best;
                accepted = true;
            } else {
                Score local_best = -INF;
                Move local_move{};
                for (int i = 0; i < root.size; ++i) {
                    Move m = root.moves[i];
                    if (!pos.make_move(m)) continue;
                    Score score = -negamax(pos, depth - 1, -beta, -alpha, 1);
                    pos.unmake_move();
                    if (score > local_best) { local_best = score; local_move = m; }
                    if (score > alpha) alpha = score;
                    if (alpha >= beta) break;
                    if (time_up()) break;
                }

                if (depth >= 4 && local_best <= alpha0) { previous = local_best; window *= 2; continue; }
                if (depth >= 4 && local_best >= beta0) { previous = local_best; window *= 2; continue; }
                best = local_move;
                best_score = local_best;
                previous = local_best;
                accepted = true;
            }
        }
        r.best_move = best;
        r.score = best_score;
        r.depth = depth;
        if (time_up()) break;
    }

    r.nodes = nodes_;
    r.pv = extract_pv(pos, r.depth);
    return r;
}

} // namespace chess
