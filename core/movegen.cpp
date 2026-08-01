#include "movegen.h"
#include "bitboard.h"
#include <algorithm>

namespace chess {
namespace {

constexpr int PT_PAWN = 1, PT_KNIGHT = 2, PT_BISHOP = 3, PT_ROOK = 4, PT_QUEEN = 5, PT_KING = 6;

constexpr int SEE_VALUE[7] = { 0, 100, 320, 330, 500, 900, 20000 };

inline int type_of(Piece p) {
    int i = piece_index(p);
    return i < 0 ? 0 : (i % 6) + 1;
}
inline int piece_val(Piece p) { return SEE_VALUE[type_of(p)]; }

inline void add_promos(MoveList& list, int from, int to, bool capture) {
    const std::uint32_t base = FLAG_PROMOTION | (capture ? FLAG_CAPTURE : 0u);
    list.push(Move::make(from, to, base, 4)); // queen first - helps ordering
    list.push(Move::make(from, to, base, 3));
    list.push(Move::make(from, to, base, 2));
    list.push(Move::make(from, to, base, 1));
}

} // anon

void generate_moves(Position& pos, MoveList& list, bool captures_only) {
    list.clear();
    MoveList pseudo;

    const Color us = pos.side_to_move();
    const Color them = opposite(us);
    const int uc = static_cast<int>(us);
    const Bitboard occ = pos.occupancy_all();
    const Bitboard mine = pos.occupancy(us);
    const Bitboard theirs = pos.occupancy(them);
    const Bitboard targets = captures_only ? theirs : ~mine;

    // ---- pawns ----
    Bitboard pawns = pos.pieces(us, PT_PAWN);
    const int push = (us == Color::White) ? 8 : -8;
    const int startRank = (us == Color::White) ? 1 : 6;
    const int promoRank = (us == Color::White) ? 6 : 1;
    while (pawns) {
        const int from = ::Bitboard::pop_lsb(pawns);
        const int r = rank_of(from);
        if (!captures_only) {
            const int one = from + push;
            if (one >= 0 && one < 64 && !((occ >> one) & 1ULL)) {
                if (r == promoRank) add_promos(pseudo, from, one, false);
                else {
                    pseudo.push(Move::make(from, one));
                    if (r == startRank) {
                        const int two = one + push;
                        if (!((occ >> two) & 1ULL)) pseudo.push(Move::make(from, two, FLAG_DOUBLE_PUSH));
                    }
                }
            }
        }
        Bitboard atk = ::Bitboard::pawn_attacks[uc][from];
        Bitboard caps = atk & theirs;
        while (caps) {
            const int to = ::Bitboard::pop_lsb(caps);
            if (r == promoRank) add_promos(pseudo, from, to, true);
            else pseudo.push(Move::make(from, to, FLAG_CAPTURE));
        }
        if (pos.ep_square() >= 0 && (atk & (1ULL << pos.ep_square())))
            pseudo.push(Move::make(from, pos.ep_square(), FLAG_CAPTURE | FLAG_EN_PASSANT));
    }

    // ---- knights / king ----
    auto leapers = [&](Bitboard bb, const uint64_t* table) {
        while (bb) {
            const int from = ::Bitboard::pop_lsb(bb);
            Bitboard to_bb = table[from] & targets;
            while (to_bb) {
                const int to = ::Bitboard::pop_lsb(to_bb);
                pseudo.push(Move::make(from, to, ((theirs >> to) & 1ULL) ? FLAG_CAPTURE : 0u));
            }
        }
    };
    leapers(pos.pieces(us, PT_KNIGHT), ::Bitboard::knight_attacks);
    leapers(pos.pieces(us, PT_KING), ::Bitboard::king_attacks);

    // ---- sliders via magic bitboards (no more loop-based ray walks) ----
    auto sliders = [&](Bitboard bb, bool rookLike, bool bishopLike) {
        while (bb) {
            const int from = ::Bitboard::pop_lsb(bb);
            Bitboard to_bb = 0;
            if (rookLike)   to_bb |= ::Bitboard::rook_attacks(from, occ);
            if (bishopLike) to_bb |= ::Bitboard::bishop_attacks(from, occ);
            to_bb &= targets;
            while (to_bb) {
                const int to = ::Bitboard::pop_lsb(to_bb);
                pseudo.push(Move::make(from, to, ((theirs >> to) & 1ULL) ? FLAG_CAPTURE : 0u));
            }
        }
    };
    sliders(pos.pieces(us, PT_BISHOP), false, true);
    sliders(pos.pieces(us, PT_ROOK),   true,  false);
    sliders(pos.pieces(us, PT_QUEEN),  true,  true);

    // ---- castling ----
    if (!captures_only) {
        const int ksq = pos.king_square(us);
        if (us == Color::White && ksq == 4 && !pos.in_check(us)) {
            if ((pos.castling_rights() & WHITE_KINGSIDE) && !((occ >> 5) & 1ULL) && !((occ >> 6) & 1ULL)
                && !pos.square_attacked(5, them) && !pos.square_attacked(6, them))
                pseudo.push(Move::make(4, 6, FLAG_KING_CASTLE));
            if ((pos.castling_rights() & WHITE_QUEENSIDE) && !((occ >> 3) & 1ULL) && !((occ >> 2) & 1ULL) && !((occ >> 1) & 1ULL)
                && !pos.square_attacked(3, them) && !pos.square_attacked(2, them))
                pseudo.push(Move::make(4, 2, FLAG_QUEEN_CASTLE));
        }
        if (us == Color::Black && ksq == 60 && !pos.in_check(us)) {
            if ((pos.castling_rights() & BLACK_KINGSIDE) && !((occ >> 61) & 1ULL) && !((occ >> 62) & 1ULL)
                && !pos.square_attacked(61, them) && !pos.square_attacked(62, them))
                pseudo.push(Move::make(60, 62, FLAG_KING_CASTLE));
            if ((pos.castling_rights() & BLACK_QUEENSIDE) && !((occ >> 59) & 1ULL) && !((occ >> 58) & 1ULL) && !((occ >> 57) & 1ULL)
                && !pos.square_attacked(59, them) && !pos.square_attacked(58, them))
                pseudo.push(Move::make(60, 58, FLAG_QUEEN_CASTLE));
        }
    }

    // ---- legality filter: bitboard test, no make/unmake churn ----
    for (int i = 0; i < pseudo.size; ++i)
        if (pos.legal_pseudo(pseudo.moves[i])) list.push(pseudo.moves[i]);
}

// ---------------------------------------------------------------------------
// Real static exchange evaluation (swap-off algorithm) instead of the old
// MVV-LVA approximation.
// ---------------------------------------------------------------------------
bool see_ge(const Position& pos, Move m, int threshold) {
    const int from = m.from(), to = m.to();
    const Piece movingP = pos.piece_at(from);
    if (movingP == Piece::None) return 0 >= threshold;

    const bool isEp = (m.flags() & FLAG_EN_PASSANT) != 0;
    Piece capturedP = isEp
        ? (pos.side_to_move() == Color::White ? Piece::BP : Piece::WP)
        : pos.piece_at(to);

    int swap = piece_val(capturedP) - threshold;
    if (m.flags() & FLAG_PROMOTION) {
        // promotion also swaps the pawn for the promoted piece
        swap += SEE_VALUE[PT_QUEEN] - SEE_VALUE[PT_PAWN];
    }
    if (swap < 0) return false;

    int nextVal = (m.flags() & FLAG_PROMOTION) ? SEE_VALUE[PT_QUEEN] : piece_val(movingP);
    swap = nextVal - swap;
    if (swap <= 0) return true;

    Bitboard occ = pos.occupancy_all();
    occ ^= (1ULL << from);
    occ |= (1ULL << to);
    if (isEp) occ &= ~(1ULL << (pos.side_to_move() == Color::White ? to - 8 : to + 8));

    Color stm = opposite(pos.side_to_move());
    Bitboard attackers = pos.attackers_to(to, occ) & occ;
    bool result = true;

    const Bitboard bishopsQ = pos.pieces(Piece::WB) | pos.pieces(Piece::BB)
                            | pos.pieces(Piece::WQ) | pos.pieces(Piece::BQ);
    const Bitboard rooksQ   = pos.pieces(Piece::WR) | pos.pieces(Piece::BR)
                            | pos.pieces(Piece::WQ) | pos.pieces(Piece::BQ);

    while (true) {
        Bitboard myAtt = attackers & pos.occupancy(stm) & occ;
        if (!myAtt) break;

        // pick the least valuable attacker
        int pt = 0;
        Bitboard bb = 0;
        for (int t = PT_PAWN; t <= PT_KING; ++t) {
            bb = myAtt & pos.pieces(stm, t);
            if (bb) { pt = t; break; }
        }
        if (!pt) break;

        const int sq = ::Bitboard::lsb_index(bb);
        occ ^= (1ULL << sq);

        // x-ray: recompute sliding attackers through the vacated square
        if (pt == PT_PAWN || pt == PT_BISHOP || pt == PT_QUEEN)
            attackers |= ::Bitboard::bishop_attacks(to, occ) & bishopsQ;
        if (pt == PT_ROOK || pt == PT_QUEEN)
            attackers |= ::Bitboard::rook_attacks(to, occ) & rooksQ;
        attackers &= occ;

        result = !result;
        stm = opposite(stm);

        swap = SEE_VALUE[pt] - swap;
        if (swap < 0) {
            // king capture is only legal if the square is then undefended
            if (pt == PT_KING && (attackers & pos.occupancy(stm) & occ)) result = !result;
            break;
        }
    }
    return result;
}

int see(const Position& pos, Move m) {
    if (!(m.flags() & FLAG_CAPTURE) && !(m.flags() & FLAG_PROMOTION)) return 0;
    // binary search the exact SEE value using see_ge - cheap enough and exact.
    int lo = -2000, hi = 2000;
    while (lo < hi) {
        const int mid = lo + (hi - lo + 1) / 2;
        if (see_ge(pos, m, mid)) lo = mid; else hi = mid - 1;
    }
    return lo;
}

uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;
    MoveList list;
    generate_moves(pos, list, false);
    if (depth == 1) return list.size;
    uint64_t nodes = 0;
    for (int i = 0; i < list.size; ++i) {
        if (!pos.make_move(list.moves[i])) continue;
        nodes += perft(pos, depth - 1);
        pos.unmake_move();
    }
    return nodes;
}

} // namespace chess
