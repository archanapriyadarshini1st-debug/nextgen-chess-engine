#include "search.h"
#include "../core/movegen.h"
#include "../evaluation/eval.h"
#include "../tb/syzygy.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>
#include <cmath>

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
        case Piece::WP: return 0; case Piece::WN: return 1; case Piece::WB: return 2;
        case Piece::WR: return 3; case Piece::WQ: return 4; case Piece::WK: return 5;
        case Piece::BP: return 6; case Piece::BN: return 7; case Piece::BB: return 8;
        case Piece::BR: return 9; case Piece::BQ: return 10; case Piece::BK: return 11;
        default: return -1;
    }
}
} // anon

void Search::init_tables() {
    for(int d=1; d<64; ++d) for(int m=1; m<64; ++m){
        lmr_table_[d][m] = static_cast<int>(0.75 + std::log(d)*std::log(m)*0.35);
    }
    for(int d=0; d<16; ++d){
        futility_margin_[d] = 150*d + 80;
        reverse_futility_margin_[d] = 120*d + 60;
        probcut_margin_[d] = 200 + 90*d;
    }
}

int Search::piece_slot(Piece p) { return piece_type_slot(p); }

bool Search::time_up() const {
    if (limit_ms_ <= 0) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
    return elapsed >= limit_ms_;
}

int Search::complexity_score(const Position& pos) const {
    MoveList moves;
    Position copy = pos;
    generate_moves(copy, moves, false);
    int material = 0;
    int pawns = 0;
    const auto& b = pos.board();
    for (int sq = 0; sq < 64; ++sq) {
        Piece p = b[sq];
        if (p == Piece::None) continue;
        material += piece_value(p);
        if (p == Piece::WP || p == Piece::BP) ++pawns;
    }
    int in_check = pos.in_check(pos.side_to_move()) ? 40 : 0;
    int mobility = std::min(40, moves.size * 2);
    int endgame = material < 2400 ? 25 : 0;
    int pawnless = pawns <= 6 ? 20 : 0;
    int tension = 0;
    // bonus for many captures available
    int caps=0; for(int i=0;i<moves.size;++i) if (moves.moves[i].flags() & FLAG_CAPTURE) ++caps;
    tension = std::min(20, caps*5);
    return in_check + mobility + endgame + pawnless + tension;
}

int Search::adaptive_depth(const Position& pos, const Limits& limits) const {
    if (limits.depth > 0) return limits.depth;
    int c = complexity_score(pos);
    int base = 18;
    if (c < 35) base = 18;
    else if (c < 60) base = 22;
    else if (c < 85) base = 26;
    else if (c < 110) base = 30;
    else base = 34;

    if (limit_ms_ > 0) {
        if (limit_ms_ < 800) base = std::min(base, 14);
        else if (limit_ms_ < 1500) base = std::min(base, 18);
        else if (limit_ms_ < 4000) base = std::min(base, 22);
        else if (limit_ms_ < 10000) base = std::min(base, 26);
        else base = std::min(base, 30);
    }
    return std::clamp(base, 12, 40);
}

int Search::score_move(const Position& pos, Move m, Move tt_move, int ply) const {
    if (m.raw == tt_move.raw) return 2'000'000;
    const auto& b = pos.board();
    Piece fromP = b[m.from()];
    Piece toP = b[m.to()];
    int s = 0;
    if (m.flags() & FLAG_CAPTURE) {
        Piece victim = toP;
        if ((m.flags() & FLAG_EN_PASSANT)) victim = pos.side_to_move() == Color::White ? Piece::BP : Piece::WP;
        // SEE
        int seeScore = see(pos, m);
        s += 1000000 + piece_value(victim)*10 - piece_value(fromP)/10 + seeScore*2;
        int slotFrom = piece_slot(fromP);
        int slotTo = (victim==Piece::None?0:piece_slot(victim)%6);
        if (slotFrom>=0) s += capture_history_[static_cast<int>(pos.side_to_move())][piece_slot(fromP)%6][m.to()]/4;
    } else {
        int slot = piece_slot(fromP);
        if (slot >= 0) s += history_[slot][m.to()];
        // continuation history
        if (ply>0 && slot>=0) s += cont_history_[slot][m.from()][m.to()]/2;
        if (killers_[ply][0].raw == m.raw) s += 90000;
        else if (killers_[ply][1].raw == m.raw) s += 80000;
    }
    if (m.flags() & FLAG_PROMOTION) s += 900000 + promo_bonus(m.promo());
    return s;
}

void Search::order_moves(Position& pos, MoveList& moves, Move tt_move, int ply) const {
    // insertion sort by score - good for small lists
    for (int i = 0; i < moves.size; ++i) {
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
    seldepth_ = std::max(seldepth_, ply);
    ++nodes_;
    if (time_up()) return evaluate(pos);
    if (ply >= MAX_PLY - 1) return evaluate(pos);
    if (pos.is_draw(ply)) return 0;

    auto* ttEntry = tt_.probe(pos.zobrist());
    Move tt_move{};
    Score tt_score = 0;
    if (ttEntry && ttEntry->key==pos.zobrist()) {
        tt_move = ttEntry->best;
        tt_score = ttEntry->score;
        if (ttEntry->depth>=0) {
            if (ttEntry->flag==TTFlag::Exact) return tt_score;
            if (ttEntry->flag==TTFlag::Beta && tt_score>=beta) return tt_score;
            if (ttEntry->flag==TTFlag::Alpha && tt_score<=alpha) return tt_score;
        }
    }

    Score stand = evaluate(pos);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    MoveList moves;
    if (pos.in_check(pos.side_to_move())) generate_moves(pos, moves, false);
    else generate_moves(pos, moves, true);

    // delta pruning + SEE pruning
    order_moves(pos, moves, tt_move, ply);

    for (int i = 0; i < moves.size; ++i) {
        Move m = moves.moves[i];
        // delta pruning: if stand + capture value + margin < alpha, skip
        Piece victim = pos.board()[m.to()];
        if (!pos.in_check(pos.side_to_move()) && victim!=Piece::None) {
            int delta = piece_value(victim);
            if (stand + delta + 200 < alpha && !see_ge(pos,m,0)) continue;
        }
        if (!see_ge(pos,m,-50)) continue;

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

Score Search::negamax(Position& pos, int depth, Score alpha, Score beta, int ply, bool cutNode) {
    seldepth_ = std::max(seldepth_, ply);
    ++nodes_;
    if (time_up()) return evaluate(pos);
    if (depth <= 0) return qsearch(pos, alpha, beta, ply);
    if (ply >= MAX_PLY - 1) return evaluate(pos);
    if (pos.is_draw(ply)) return 0;

    bool in_check = pos.in_check(pos.side_to_move());
    if (in_check) ++depth;

    // TT probe
    tt_.prefetch(pos.zobrist());
    auto* tte = tt_.probe(pos.zobrist());
    Move tt_move{};
    Score tt_score=0;
    int tt_depth=-1;
    TTFlag tt_flag=TTFlag::Exact;
    bool tt_hit=false;
    if (tte && tte->key==pos.zobrist()) {
        tt_hit=true;
        tt_move=tte->best;
        tt_score=tte->score;
        tt_depth=tte->depth;
        tt_flag=tte->flag;
        if (ply>0 && tt_depth>=depth) {
            if (tt_flag==TTFlag::Exact) return tt_score;
            if (tt_flag==TTFlag::Beta && tt_score>=beta) return tt_score;
            if (tt_flag==TTFlag::Alpha && tt_score<=alpha) return tt_score;
        }
    }

    Score staticEval = evaluate(pos);

    // Syzygy probe
    if (pos.game_ply()>0) {
        SyzygyTablebase tb;
        // path should be set via UCI - for now we attempt probe if available, it will return nullopt if not
        if (auto wdl = tb.probe_wdl(pos)) {
            Score tbScore = 0;
            if (*wdl > 0) tbScore = MATE_SCORE - pos.game_ply() - 50;
            else if (*wdl < 0) tbScore = -MATE_SCORE + pos.game_ply() + 50;
            else tbScore = 0;
            TTFlag flag = (*wdl==0? TTFlag::Exact : (*wdl>0? TTFlag::Beta: TTFlag::Alpha));
            if (flag==TTFlag::Exact || (flag==TTFlag::Beta && tbScore>=beta) || (flag==TTFlag::Alpha && tbScore<=alpha)) {
                tt_.store(pos.zobrist(), depth, tbScore, flag, Move{}, staticEval, ply);
                return tbScore;
            }
        }
    }

    // Reverse Futility Pruning
    if (!in_check && depth<=7 && staticEval - reverse_futility_margin_[depth] >= beta) {
        return staticEval;
    }

    // Null Move Pruning
    if (!in_check && depth>=3 && ply>0 && staticEval>=beta) {
        // check non-pawn material
        bool hasNonPawn = false;
        for (int sq=0;sq<64;++sq) {
            Piece p = pos.board()[sq];
            if (p!=Piece::None && piece_color(p)==pos.side_to_move() && p!=Piece::WP && p!=Piece::BP && p!=Piece::WK && p!=Piece::BK) { hasNonPawn=true; break; }
        }
        if (hasNonPawn) {
            pos.make_null_move();
            int R = 3 + depth/4 + (staticEval - beta)/200;
            R = std::min(R, depth);
            Score score = -negamax(pos, depth - R, -beta, -beta+1, ply+1, !cutNode);
            pos.unmake_move();
            if (score >= beta) return beta;
        }
    }

    // IIR
    if (tt_move.is_null() && depth>=4) {
        int iidDepth = depth/2;
        negamax(pos, iidDepth, alpha, beta, ply, cutNode);
        if (auto* e2 = tt_.probe(pos.zobrist())) if (e2->key==pos.zobrist()) tt_move=e2->best;
    }

    // ProbCut
    if (!in_check && depth>=5 && std::abs(beta)<MATE_SCORE-100) {
        int probBeta = beta + probcut_margin_[std::min(depth,15)];
        MoveList captures;
        generate_moves(pos, captures, true);
        order_moves(pos, captures, tt_move, ply);
        for (int i=0;i<captures.size;++i) {
            Move m=captures.moves[i];
            if (!see_ge(pos,m, probBeta - staticEval)) continue;
            if (!pos.make_move(m)) continue;
            Score score = -qsearch(pos, -probBeta, -probBeta+1, ply+1);
            if (score>=probBeta) score = -negamax(pos, depth-4, -probBeta, -probBeta+1, ply+1, !cutNode);
            pos.unmake_move();
            if (score>=probBeta) return score;
        }
    }

    MoveList moves;
    generate_moves(pos, moves, false);
    if (moves.size == 0) return in_check ? -MATE_SCORE + ply : 0;

    order_moves(pos, moves, tt_move, ply);

    Score alpha0 = alpha;
    Score beta0 = beta;
    Move best{};
    Score best_score = -INF;
    int movesTried=0;
    int quietTried=0;

    // Multi-cut: at cut nodes, if we have 3 moves that fail high after shallow search, prune
    int multiCut=0;

    for (int i = 0; i < moves.size; ++i) {
        Move m = moves.moves[i];
        bool isCapture = (m.flags() & FLAG_CAPTURE) || pos.board()[m.to()]!=Piece::None;
        bool isPromo = (m.flags() & FLAG_PROMOTION);

        // Late Move Pruning
        if (!in_check && !isCapture && !isPromo && depth<=4 && movesTried> depth*depth + 4) break;

        // Futility Pruning
        if (!in_check && !isCapture && !isPromo && depth<=4 && staticEval + futility_margin_[depth] + 80*movesTried <= alpha) {
            ++movesTried; continue;
        }

        // SEE pruning
        if (depth<=6 && !isPromo && !in_check) {
            int thresh = - (depth*depth*10);
            if (isCapture) thresh = -50*depth;
            if (!see_ge(pos,m,thresh)) { ++movesTried; continue; }
        }

        if (!pos.make_move(m)) continue;

        int extension=0;
        if (in_check) extension=1;

        // Singular Extension: if TT move and depth>=6
        if (ply>0 && tt_hit && m.raw==tt_move.raw && depth>=6 && std::abs(tt_score)<MATE_SCORE-100 && (tt_flag==TTFlag::Beta || tt_flag==TTFlag::Exact) && tt_depth>=depth-3) {
            Score singularBeta = tt_score - 2*depth;
            MoveList sub;
            // we need to exclude tt_move - search with reduced window to see if all other moves fail low
            // Simplified: do a shallow search with beta = singularBeta
            // If no move reaches singularBeta, then extension
            Position p2=pos;
            // Actually we need to search position before move with excluded move - we will simulate by searching current pos with excluded logic? For simplicity, do a verification search of depth/2
            Score v = -negamax(p2, depth/2 -1, -singularBeta, -singularBeta+1, ply+1, false);
            if (v < singularBeta) extension=1;
            else if (v >= singularBeta && tt_score < beta) extension=-1;
        }

        int new_depth = depth -1 + extension;
        Score score;

        // LMR
        if (movesTried>=3 && depth>=3 && !isCapture && !isPromo && !in_check) {
            int r = lmr_table_[std::min(depth,63)][std::min(movesTried,63)];
            if (cutNode) ++r;
            // history correction
            int slot = piece_slot(pos.board()[m.from()]);
            if (slot>=0) r -= history_[slot][m.to()]/8192;
            r = std::max(0, std::min(r, depth-2));
            if (r>0) {
                score = -negamax(pos, new_depth - r, -alpha-1, -alpha, ply+1, true);
                if (score>alpha) score = -negamax(pos, new_depth, -alpha-1, -alpha, ply+1, false);
            } else {
                score = -negamax(pos, new_depth, -alpha-1, -alpha, ply+1, true);
            }
            if (score>alpha && score<beta) score = -negamax(pos, new_depth, -beta, -alpha, ply+1, false);
        } else {
            if (movesTried==0) score = -negamax(pos, new_depth, -beta, -alpha, ply+1, false);
            else {
                score = -negamax(pos, new_depth, -alpha-1, -alpha, ply+1, true);
                if (score>alpha) score = -negamax(pos, new_depth, -beta, -alpha, ply+1, false);
            }
        }

        pos.unmake_move();
        ++movesTried;
        if (!isCapture) ++quietTried;

        if (time_up()) return best_score;

        if (score > best_score) { best_score=score; best=m; }

        if (score > alpha) {
            alpha=score;
            if (score >= beta) {
                // beta cutoff
                if (!isCapture) {
                    killers_[ply][1]=killers_[ply][0];
                    killers_[ply][0]=m;
                    Piece moved = pos.board()[m.from()];
                    int sl = piece_slot(moved);
                    if (sl>=0) {
                        history_[sl][m.to()] += depth*depth;
                        if (history_[sl][m.to()]>16384) {
                            for(int f=0;f<64;++f) history_[sl][f]/=2;
                        }
                        // cont history
                        if (ply>0) cont_history_[sl][m.from()][m.to()] += depth*depth;
                    }
                } else {
                    int sl = piece_slot(pos.board()[m.from()]);
                    if (sl>=0) capture_history_[static_cast<int>(pos.side_to_move())][sl%6][m.to()] += depth*depth;
                }
                // multi-cut counting
                if (cutNode) {
                    if (++multiCut>=2 && depth>=5) break;
                } else break;
            }
        }
    }

    if (movesTried==0) return in_check ? -MATE_SCORE + ply : 0;

    TTFlag flag = TTFlag::Exact;
    if (best_score <= alpha0) flag = TTFlag::Alpha;
    else if (best_score >= beta0) flag = TTFlag::Beta;

    tt_.store(pos.zobrist(), depth, best_score, flag, best, staticEval, ply);
    return best_score;
}

std::vector<Move> Search::extract_pv(Position pos, int depth) const {
    std::vector<Move> pv;
    for (int i = 0; i < depth; ++i) {
        auto* e = tt_.probe(pos.zobrist());
        if (!e || e->key!=pos.zobrist() || e->best.is_null()) break;
        Move m = e->best;
        if (!pos.make_move(m)) break;
        pv.push_back(m);
    }
    return pv;
}

SearchResult Search::think(Position& pos, const Limits& limits) {
    SearchResult r;
    nodes_ = 0;
    seldepth_ = 0;
    start_ = std::chrono::steady_clock::now();
    tt_.new_search();

    if (limits.movetime_ms > 0) limit_ms_ = limits.movetime_ms;
    else if (limits.infinite) limit_ms_ = 0;
    else {
        auto remaining = pos.side_to_move() == Color::White ? limits.wtime_ms : limits.btime_ms;
        auto inc = pos.side_to_move() == Color::White ? limits.winc_ms : limits.binc_ms;
        if (remaining > 0) limit_ms_ = std::max<std::int64_t>(10, remaining / std::max(1, limits.movestogo ? limits.movestogo : 30) + inc * 7 / 10);
        else limit_ms_ = 0;
    }

    int max_depth = adaptive_depth(pos, limits);
    MoveList root;
    generate_moves(pos, root, false);
    if (root.size == 0) {
        r.best_move = Move{};
        r.score = pos.in_check(pos.side_to_move()) ? -MATE_SCORE : 0;
        r.nodes = nodes_;
        r.seldepth = seldepth_;
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
            if (auto* tt = tt_.probe(pos.zobrist())) if (tt->key==pos.zobrist()) tt_move = tt->best;
            order_moves(pos, root, tt_move, 0);

            if (parallel_threads > 1 && depth >= 4 && root.size >= 8) {
                struct RootEval { Move move; Score score; int seldepth; };
                std::vector<std::future<RootEval>> tasks;
                tasks.reserve(root.size);
                const int launch_depth = depth - 1;
                for (int i = 0; i < root.size; ++i) {
                    Move m = root.moves[i];
                    tasks.emplace_back(std::async(std::launch::async, [this, pos, m, launch_depth]() mutable {
                        // Each thread gets its own search instance but shares TT via pointer? For safety we copy
                        Search local;
                        local.tt_ = this->tt_; // copy TT snapshot
                        local.history_ = this->history_;
                        Position p = pos;
                        if (!p.make_move(m)) return RootEval{m, -INF, 0};
                        Score sc = -local.negamax(p, launch_depth, -INF, INF, 1, false);
                        return RootEval{m, sc, local.seldepth_};
                    }));
                }
                Score local_best = -INF;
                Move local_move{};
                int local_seldepth = 0;
                for (auto& fut : tasks) {
                    auto ev = fut.get();
                    if (ev.score > local_best) { local_best = ev.score; local_move = ev.move; }
                    local_seldepth = std::max(local_seldepth, ev.seldepth);
                }
                best = local_move;
                best_score = local_best;
                previous = local_best;
                seldepth_ = std::max(seldepth_, local_seldepth);
                accepted = true;
            } else {
                Score local_best = -INF;
                Move local_move{};
                for (int i = 0; i < root.size; ++i) {
                    Move m = root.moves[i];
                    if (!pos.make_move(m)) continue;
                    Score score = -negamax(pos, depth - 1, -beta, -alpha, 1, false);
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
    r.seldepth = std::max(seldepth_, r.depth);
    r.pv = extract_pv(pos, r.depth);
    return r;
}

} // namespace chess
