#include "eval.h"
#include "../core/bitboard.h"
#include "../nnue/nnue.h"
#include "stockfish_nnue.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace chess {
namespace {

constexpr int TEMPO = 8;

constexpr int PT_PAWN = 1, PT_KNIGHT = 2, PT_BISHOP = 3, PT_ROOK = 4, PT_QUEEN = 5, PT_KING = 6;

int value(Piece p) {
    switch (p) {
        case Piece::WP: case Piece::BP: return 100;
        case Piece::WN: case Piece::BN: return 320;
        case Piece::WB: case Piece::BB: return 330;
        case Piece::WR: case Piece::BR: return 500;
        case Piece::WQ: case Piece::BQ: return 900;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// PST is defined ONLY from White's point of view and Black reads it through a
// vertically mirrored square. The old version recomputed "center" from the raw
// rank for both colours, and |3-r| is not mirror symmetric across r -> 7-r,
// which is where most of the (up to 74cp) asymmetry came from.
// ---------------------------------------------------------------------------
int pst_white(int type, int sq) {
    const int f = file_of(sq), r = rank_of(sq);
    const int center = 14 - (std::abs(3 - f) + std::abs(3 - r)) * 2;
    switch (type) {
        case PT_PAWN:   return r * 9 + center / 2;
        case PT_KNIGHT: return center * 3;
        case PT_BISHOP: return center * 2 + ((f == 0 || f == 7) ? -2 : 0);
        case PT_ROOK:   return (f == 0 || f == 7) ? 8 : center;
        case PT_QUEEN:  return center;
        case PT_KING:   return -r * 5 + center / 3;
        default:        return 0;
    }
}
inline int pst(Color c, int type, int sq) {
    return pst_white(type, c == Color::White ? sq : mirror_square(sq));
}

inline Bitboard pawn_attack_span(const Position& pos, Color c) {
    Bitboard att = 0, pawns = pos.pieces(c, PT_PAWN);
    const int ci = static_cast<int>(c);
    while (pawns) att |= ::Bitboard::pawn_attacks[ci][::Bitboard::pop_lsb(pawns)];
    return att;
}

// relative rank: 0 = own back rank for both colours
inline int rel_rank(Color c, int sq) { return c == Color::White ? rank_of(sq) : 7 - rank_of(sq); }

int evaluate_pawns(const Position& pos, Color c) {
    int score = 0;
    const Bitboard pawns = pos.pieces(c, PT_PAWN);
    const Bitboard enemy = pos.pieces(opposite(c), PT_PAWN);

    int fileCount[8] = {0};
    Bitboard t = pawns;
    while (t) fileCount[file_of(::Bitboard::pop_lsb(t))]++;

    t = pawns;
    while (t) {
        const int sq = ::Bitboard::pop_lsb(t);
        const int f = file_of(sq);
        const int rr = rel_rank(c, sq);

        bool iso = !(f > 0 && fileCount[f - 1]) && !(f < 7 && fileCount[f + 1]);
        if (iso) score -= 15;
        if (fileCount[f] > 1) score -= 6;

        // connected / phalanx (mirror safe: uses relative rank)
        bool connected = false;
        for (int df = -1; df <= 1; df += 2) {
            const int nf = f + df;
            if (nf < 0 || nf > 7) continue;
            for (int drr = -1; drr <= 1; ++drr) {
                const int nrr = rr + drr;
                if (nrr < 0 || nrr > 7) continue;
                const int nr = (c == Color::White) ? nrr : 7 - nrr;
                if ((pawns >> square_of(nf, nr)) & 1ULL) { connected = true; break; }
            }
            if (connected) break;
        }
        if (connected) score += 5;

        // passed / candidate
        int enemyAhead = 0, friendlyAhead = 0;
        for (int nrr = rr + 1; nrr <= 7; ++nrr) {
            const int nr = (c == Color::White) ? nrr : 7 - nrr;
            for (int df = -1; df <= 1; ++df) {
                const int nf = f + df;
                if (nf < 0 || nf > 7) continue;
                const int ts = square_of(nf, nr);
                if ((enemy >> ts) & 1ULL) ++enemyAhead;
                if ((pawns >> ts) & 1ULL) ++friendlyAhead;
            }
        }
        if (enemyAhead == 0) score += 20 + rr * 4;
        else if (enemyAhead == 1 && friendlyAhead == 0) score += 8;
        else if (iso) score -= 10;
    }
    return score;
}

int evaluate_king_safety(const Position& pos, Color c) {
    const int ks = pos.king_square(c);
    if (ks < 0) return 0;
    const int f = file_of(ks), r = rank_of(ks);
    const int dir = (c == Color::White) ? 1 : -1;
    const Bitboard own = pos.pieces(c, PT_PAWN);

    int shield = 0;
    for (int df = -1; df <= 1; ++df)
        for (int dr = 1; dr <= 2; ++dr) {
            const int nf = f + df, nr = r + dir * dr;
            if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
            if ((own >> square_of(nf, nr)) & 1ULL) shield += 8;
        }

    int openPenalty = 0;
    const Bitboard allPawns = pos.pieces(Piece::WP) | pos.pieces(Piece::BP);
    for (int df = -1; df <= 1; ++df) {
        const int nf = f + df;
        if (nf < 0 || nf > 7) continue;
        Bitboard fileBB = 0;
        for (int rr = 0; rr < 8; ++rr) fileBB |= 1ULL << square_of(nf, rr);
        if (!(fileBB & allPawns)) openPenalty -= 12;
        else if (!(fileBB & own)) openPenalty -= 6;
    }
    return shield + openPenalty;
}

int evaluate_outposts(const Position& pos, Color c) {
    int score = 0;
    const Bitboard ownPawnAtt = pawn_attack_span(pos, c);
    const Bitboard enemyPawnAtt = pawn_attack_span(pos, opposite(c));
    auto scan = [&](Bitboard bb, int bonus) {
        while (bb) {
            const int sq = ::Bitboard::pop_lsb(bb);
            const int rr = rel_rank(c, sq);
            if (rr < 3 || rr > 5) continue;
            if (!((ownPawnAtt >> sq) & 1ULL)) continue;
            if ((enemyPawnAtt >> sq) & 1ULL) continue;
            score += bonus;
        }
    };
    scan(pos.pieces(c, PT_KNIGHT), 25);
    scan(pos.pieces(c, PT_BISHOP), 15);
    return score;
}

int evaluate_mobility(const Position& pos, Color c) {
    const Bitboard occ = pos.occupancy_all();
    const Bitboard mine = pos.occupancy(c);
    const Bitboard bad = pawn_attack_span(pos, opposite(c)) | mine;
    int mob = 0;
    Bitboard bb = pos.pieces(c, PT_KNIGHT);
    while (bb) mob += 4 * ::Bitboard::popcount(::Bitboard::knight_attacks[::Bitboard::pop_lsb(bb)] & ~bad);
    bb = pos.pieces(c, PT_BISHOP);
    while (bb) mob += 4 * ::Bitboard::popcount(::Bitboard::bishop_attacks(::Bitboard::pop_lsb(bb), occ) & ~bad);
    bb = pos.pieces(c, PT_ROOK);
    while (bb) mob += 3 * ::Bitboard::popcount(::Bitboard::rook_attacks(::Bitboard::pop_lsb(bb), occ) & ~bad);
    bb = pos.pieces(c, PT_QUEEN);
    while (bb) mob += 1 * ::Bitboard::popcount(::Bitboard::queen_attacks(::Bitboard::pop_lsb(bb), occ) & ~bad);
    return mob;
}

int evaluate_space(const Position& pos, Color c) {
    const Bitboard occ = pos.occupancy_all();
    const Bitboard enemyPawnAtt = pawn_attack_span(pos, opposite(c));
    int space = 0;
    for (int rr = 2; rr <= 4; ++rr) {
        const int r = (c == Color::White) ? rr : 7 - rr;
        for (int f = 2; f <= 5; ++f) {
            const int sq = square_of(f, r);
            if (((occ >> sq) & 1ULL) || ((enemyPawnAtt >> sq) & 1ULL)) continue;
            ++space;
        }
    }
    return space * 2;
}

int evaluate_threats(const Position& pos, Color c) {
    int score = 0;
    const Color them = opposite(c);
    const Bitboard occ = pos.occupancy_all();
    Bitboard bb = pos.occupancy(them) & ~pos.pieces(them, PT_KING);
    while (bb) {
        const int sq = ::Bitboard::pop_lsb(bb);
        const Bitboard att = pos.attackers_to(sq, occ);
        if ((att & pos.occupancy(c)) && !(att & pos.occupancy(them))) score += 10;
    }
    return score;
}

int evaluate_rooks(const Position& pos, Color c) {
    int score = 0;
    const Bitboard allPawns = pos.pieces(Piece::WP) | pos.pieces(Piece::BP);
    const Bitboard own = pos.pieces(c, PT_PAWN);
    Bitboard bb = pos.pieces(c, PT_ROOK);
    while (bb) {
        const int sq = ::Bitboard::pop_lsb(bb);
        const int f = file_of(sq);
        Bitboard fileBB = 0;
        for (int r = 0; r < 8; ++r) fileBB |= 1ULL << square_of(f, r);
        if (!(fileBB & allPawns)) score += 18;
        else if (!(fileBB & own)) score += 10;
        if (rel_rank(c, sq) == 6) score += 20;
    }
    return score;
}

int side_score(const Position& pos, Color c) {
    int s = 0;
    s += evaluate_pawns(pos, c);
    s += evaluate_king_safety(pos, c);
    s += evaluate_outposts(pos, c);
    s += evaluate_mobility(pos, c);
    s += evaluate_space(pos, c);
    s += evaluate_threats(pos, c);
    s += evaluate_rooks(pos, c);
    for (int type = PT_PAWN; type <= PT_KING; ++type) {
        Bitboard bb = pos.pieces(c, type);
        while (bb) {
            const int sq = ::Bitboard::pop_lsb(bb);
            s += value(make_piece(c, type)) + pst(c, type, sq);
        }
    }
    if (::Bitboard::popcount(pos.pieces(c, PT_BISHOP)) >= 2) s += 32;
    return s;
}

// ---- optional external evaluator (OFF unless explicitly enabled) ----
NNUE g_nnue;
std::once_flag g_nnue_flag;
std::string g_external_path;

void try_load_nnue() {
    std::call_once(g_nnue_flag, [] {
        for (const char* p : {"networks/nnue.nnue", "nnue.nnue"})
            if (std::filesystem::exists(p) && g_nnue.load(p)) break;
        // The old code unconditionally forked three Stockfish candidates with a
        // 2s handshake timeout each - 6 seconds of startup on every launch, and
        // when it succeeded the "engine" was just proxying Stockfish's eval.
        if (const char* env = std::getenv("NGCE_EXTERNAL_EVAL")) g_external_path = env;
        if (!g_external_path.empty() && std::filesystem::exists(g_external_path))
            StockfishEvaluator::instance().init(g_external_path);
    });
}

} // anon

bool load_nnue_file(const std::string& path) { try_load_nnue(); return g_nnue.load(path); }
void set_external_eval(const std::string& path) { g_external_path = path; }

// White-relative, mirror-antisymmetric by construction.
Score evaluate_white_relative(const Position& pos) {
    int score = side_score(pos, Color::White) - side_score(pos, Color::Black);

    int totalMat = 0;
    for (int type = PT_PAWN; type <= PT_QUEEN; ++type)
        totalMat += value(make_piece(Color::White, type))
                  * (::Bitboard::popcount(pos.pieces(Color::White, type)) + ::Bitboard::popcount(pos.pieces(Color::Black, type)));
    if (totalMat < 2500) score = score * (120 + totalMat / 20) / 100;

    return static_cast<Score>(std::clamp(score, -10000, 10000));
}

Score evaluate_handcrafted(const Position& pos) {
    const int side = (pos.side_to_move() == Color::White) ? 1 : -1;
    const int v = evaluate_white_relative(pos) * side + TEMPO;   // one tempo, not two
    return static_cast<Score>(std::clamp(v, -10000, 10000));
}

Score evaluate(const Position& pos) {
    try_load_nnue();
    if (StockfishEvaluator::instance().is_available()) {
        const Score sf = StockfishEvaluator::instance().evaluate(pos);
        if (sf != 0) return std::clamp<Score>(sf, -10000, 10000);
    }
    const Score classical = evaluate_handcrafted(pos);
    if (g_nnue.is_loaded()) {
        const Score n = g_nnue.evaluate(pos);   // already side-to-move relative
        return std::clamp<Score>((n * 7 + classical * 3) / 10, -10000, 10000);
    }
    return classical;
}

} // namespace chess
