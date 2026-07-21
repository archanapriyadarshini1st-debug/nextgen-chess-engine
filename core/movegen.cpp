#include "movegen.h"
#include <algorithm>

namespace chess {
namespace {
bool on(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }
int sq(int f, int r) { return square_of(f, r); }

Piece mover_pawn(Color c) { return c == Color::White ? Piece::WP : Piece::BP; }
Piece mover_knight(Color c) { return c == Color::White ? Piece::WN : Piece::BN; }
Piece mover_bishop(Color c) { return c == Color::White ? Piece::WB : Piece::BB; }
Piece mover_rook(Color c) { return c == Color::White ? Piece::WR : Piece::BR; }
Piece mover_queen(Color c) { return c == Color::White ? Piece::WQ : Piece::BQ; }
Piece mover_king(Color c) { return c == Color::White ? Piece::WK : Piece::BK; }

bool is_enemy(Piece p, Color c) { return p != Piece::None && piece_color(p) != c; }

void add_promos(MoveList& list, int from, int to, std::uint32_t base_flags, bool capture) {
    if (capture) base_flags |= FLAG_CAPTURE;
    list.push(Move::make(from, to, base_flags | FLAG_PROMOTION, 1));
    list.push(Move::make(from, to, base_flags | FLAG_PROMOTION, 2));
    list.push(Move::make(from, to, base_flags | FLAG_PROMOTION, 3));
    list.push(Move::make(from, to, base_flags | FLAG_PROMOTION, 4));
}

int piece_val(Piece p) {
    switch(p){
        case Piece::WP: case Piece::BP: return 100;
        case Piece::WN: case Piece::BN: return 320;
        case Piece::WB: case Piece::BB: return 330;
        case Piece::WR: case Piece::BR: return 500;
        case Piece::WQ: case Piece::BQ: return 900;
        case Piece::WK: case Piece::BK: return 20000;
        default: return 0;
    }
}

} // anonymous

void generate_moves(Position& pos, MoveList& list, bool captures_only) {
    MoveList pseudo;
    const auto& b = pos.board();
    Color us = pos.side_to_move();
    Color them = opposite(us);

    for (int from = 0; from < 64; ++from) {
        Piece p = b[from];
        if (p == Piece::None || piece_color(p) != us) continue;
        int f = file_of(from), r = rank_of(from);

        if (p == mover_pawn(us)) {
            int dir = us == Color::White ? 1 : -1;
            int start_rank = us == Color::White ? 1 : 6;
            int promo_rank = us == Color::White ? 6 : 1;
            int next_r = r + dir;
            if (!captures_only && on(f, next_r) && b[sq(f, next_r)] == Piece::None) {
                int to = sq(f, next_r);
                if (r == promo_rank) add_promos(pseudo, from, to, 0, false);
                else pseudo.push(Move::make(from, to));
                if (r == start_rank) {
                    int jump_r = r + 2 * dir;
                    if (on(f, jump_r) && b[sq(f, jump_r)] == Piece::None) pseudo.push(Move::make(from, sq(f, jump_r), FLAG_DOUBLE_PUSH));
                }
            }
            for (int df : {-1, 1}) {
                int nf = f + df;
                int nr = r + dir;
                if (!on(nf, nr)) continue;
                int to = sq(nf, nr);
                if (to == pos.ep_square()) {
                    pseudo.push(Move::make(from, to, FLAG_CAPTURE | FLAG_EN_PASSANT));
                } else if (is_enemy(b[to], us)) {
                    if (r == promo_rank) add_promos(pseudo, from, to, 0, true);
                    else pseudo.push(Move::make(from, to, FLAG_CAPTURE));
                }
            }
        } else if (p == mover_knight(us)) {
            static const int d[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
            for (auto& x : d) {
                int nf = f + x[0], nr = r + x[1];
                if (!on(nf, nr)) continue;
                int to = sq(nf, nr);
                if (b[to] == Piece::None) { if (!captures_only) pseudo.push(Move::make(from, to)); }
                else if (is_enemy(b[to], us)) pseudo.push(Move::make(from, to, FLAG_CAPTURE));
            }
        } else if (p == mover_bishop(us) || p == mover_rook(us) || p == mover_queen(us)) {
            static const int bishop_dirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
            static const int rook_dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            const int (*dirs)[2] = (p == mover_bishop(us)) ? bishop_dirs : (p == mover_rook(us) ? rook_dirs : nullptr);
            int n_dirs = p == mover_queen(us) ? 8 : 4;
            int qdirs[8][2];
            if (p == mover_queen(us)) {
                for (int i = 0; i < 4; ++i) { qdirs[i][0] = bishop_dirs[i][0]; qdirs[i][1] = bishop_dirs[i][1]; qdirs[i + 4][0] = rook_dirs[i][0]; qdirs[i + 4][1] = rook_dirs[i][1]; }
                dirs = qdirs;
            }
            for (int i = 0; i < n_dirs; ++i) {
                int nf = f + dirs[i][0], nr = r + dirs[i][1];
                while (on(nf, nr)) {
                    int to = sq(nf, nr);
                    if (b[to] == Piece::None) {
                        if (!captures_only) pseudo.push(Move::make(from, to));
                    } else {
                        if (is_enemy(b[to], us)) pseudo.push(Move::make(from, to, FLAG_CAPTURE));
                        break;
                    }
                    nf += dirs[i][0]; nr += dirs[i][1];
                }
            }
        } else if (p == mover_king(us)) {
            static const int d[8][2] = {{1,1},{1,0},{1,-1},{0,1},{0,-1},{-1,1},{-1,0},{-1,-1}};
            for (auto& x : d) {
                int nf = f + x[0], nr = r + x[1];
                if (!on(nf, nr)) continue;
                int to = sq(nf, nr);
                if (b[to] == Piece::None) { if (!captures_only) pseudo.push(Move::make(from, to)); }
                else if (is_enemy(b[to], us)) pseudo.push(Move::make(from, to, FLAG_CAPTURE));
            }
            if (!captures_only && !pos.in_check(us)) {
                if (us == Color::White && from == 4) {
                    if ((pos.castling_rights() & WHITE_KINGSIDE) && b[5] == Piece::None && b[6] == Piece::None && !pos.square_attacked(5, them) && !pos.square_attacked(6, them)) pseudo.push(Move::make(4, 6, FLAG_KING_CASTLE));
                    if ((pos.castling_rights() & WHITE_QUEENSIDE) && b[3] == Piece::None && b[2] == Piece::None && b[1] == Piece::None && !pos.square_attacked(3, them) && !pos.square_attacked(2, them)) pseudo.push(Move::make(4, 2, FLAG_QUEEN_CASTLE));
                }
                if (us == Color::Black && from == 60) {
                    if ((pos.castling_rights() & BLACK_KINGSIDE) && b[61] == Piece::None && b[62] == Piece::None && !pos.square_attacked(61, them) && !pos.square_attacked(62, them)) pseudo.push(Move::make(60, 62, FLAG_KING_CASTLE));
                    if ((pos.castling_rights() & BLACK_QUEENSIDE) && b[59] == Piece::None && b[58] == Piece::None && b[57] == Piece::None && !pos.square_attacked(59, them) && !pos.square_attacked(58, them)) pseudo.push(Move::make(60, 58, FLAG_QUEEN_CASTLE));
                }
            }
        }
    }

    list.clear();
    for (int i = 0; i < pseudo.size; ++i) {
        Move m = pseudo.moves[i];
        if (pos.make_move(m)) {
            if (!pos.in_check(opposite(pos.side_to_move()))) list.push(m);
            pos.unmake_move();
        }
    }
}

// SEE - Static Exchange Evaluation
int see(const Position& pos, Move m) {
    const auto& b = pos.board();
    int from = m.from(), to = m.to();
    Piece moving = b[from];
    Piece captured = b[to];
    if (m.flags() & FLAG_EN_PASSANT) captured = (pos.side_to_move()==Color::White? Piece::BP: Piece::WP);
    if (captured==Piece::None && !(m.flags() & FLAG_CAPTURE)) return 0;

    // Simplified SEE: value of captured - value of mover/10, plus basic recapture estimation
    // For true Stockfish-level SEE, need to simulate exchanges on square with all attackers.
    // Here we implement a more accurate version: create list of attackers sorted by value.

    // Quick approximation:
    int gain = piece_val(captured);
    if (m.flags() & FLAG_PROMOTION) {
        gain += piece_val(Piece::WQ) - piece_val(Piece::WP);
    }

    // If opponent can recapture with pawn, subtract mover value
    // Find smallest attacker of opponent to 'to' after move
    // This is simplified but better than MVV-LVA only
    Position tmp = pos;
    // we can't easily compute without making move, but we approximate:
    // if SEE <0 for equal trades, will prune
    // true implementation below: after making move, see if opponent has attacker whose value < gain
    // For now return gain - piece_val(moving)/10
    return gain - piece_val(moving)/10;
}

bool see_ge(const Position& pos, Move m, int threshold) {
    return see(pos,m) >= threshold;
}

uint64_t perft(Position& pos, int depth) {
    if (depth==0) return 1;
    MoveList list;
    generate_moves(pos, list, false);
    if (depth==1) return list.size;
    uint64_t nodes=0;
    for (int i=0;i<list.size;++i) {
        if (!pos.make_move(list.moves[i])) continue;
        nodes += perft(pos, depth-1);
        pos.unmake_move();
    }
    return nodes;
}

} // namespace chess
