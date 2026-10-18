#include "eval.h"
#include "../nnue/nnue.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>

namespace chess {
namespace {
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
int pst(Piece p, int sq) {
    // a1=0 tables. Centre pawns, edge-penalised knights, king wants to castle.
    // Previous file-agnostic pawn PST scored a2a4 ≈ e2e4 (31 vs 33).
    static const int pawn_tbl[64] = {
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 10,-20,-20, 10, 10,  5,
         5, -5,-10,  0,  0,-10, -5,  5,
         0,  0,  0, 20, 20,  0,  0,  0,
         5,  5, 10, 25, 25, 10,  5,  5,
        10, 10, 20, 30, 30, 20, 10, 10,
        50, 50, 50, 50, 50, 50, 50, 50,
         0,  0,  0,  0,  0,  0,  0,  0
    };
    static const int knight_tbl[64] = {
        -50,-40,-30,-30,-30,-30,-40,-50,
        -40,-20,  0,  5,  5,  0,-20,-40,
        -30,  5, 10, 15, 15, 10,  5,-30,
        -30,  0, 15, 20, 20, 15,  0,-30,
        -30,  5, 15, 20, 20, 15,  5,-30,
        -30,  0, 10, 15, 15, 10,  0,-30,
        -40,-20,  0,  0,  0,  0,-20,-40,
        -50,-40,-30,-30,-30,-30,-40,-50
    };
    static const int bishop_tbl[64] = {
        -20,-10,-10,-10,-10,-10,-10,-20,
        -10,  5,  0,  0,  0,  0,  5,-10,
        -10, 10, 10, 10, 10, 10, 10,-10,
        -10,  0, 10, 10, 10, 10,  0,-10,
        -10,  5,  5, 10, 10,  5,  5,-10,
        -10,  0,  5, 10, 10,  5,  0,-10,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -20,-10,-10,-10,-10,-10,-10,-20
    };
    static const int rook_tbl[64] = {
         0,  0,  0,  5,  5,  0,  0,  0,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
         5, 10, 10, 10, 10, 10, 10,  5,
         0,  0,  0,  0,  0,  0,  0,  0
    };
    static const int queen_tbl[64] = {
        -20,-10,-10, -5, -5,-10,-10,-20,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -10,  0,  5,  5,  5,  5,  0,-10,
         -5,  0,  5,  5,  5,  5,  0, -5,
          0,  0,  5,  5,  5,  5,  0, -5,
        -10,  5,  5,  5,  5,  5,  0,-10,
        -10,  0,  5,  0,  0,  0,  0,-10,
        -20,-10,-10, -5, -5,-10,-10,-20
    };
    static const int king_mg[64] = {
         20, 30, 10,  0,  0, 10, 30, 20,
         20, 20,  0,  0,  0,  0, 20, 20,
        -10,-20,-20,-20,-20,-20,-20,-10,
        -20,-30,-30,-40,-40,-30,-30,-20,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30
    };
    auto lookup = [&](const int* tbl, bool white) {
        int s = white ? sq : (sq ^ 56);
        return tbl[s];
    };
    switch (p) {
        case Piece::WP: return lookup(pawn_tbl, true);
        case Piece::BP: return lookup(pawn_tbl, false);
        case Piece::WN: return lookup(knight_tbl, true);
        case Piece::BN: return lookup(knight_tbl, false);
        case Piece::WB: return lookup(bishop_tbl, true);
        case Piece::BB: return lookup(bishop_tbl, false);
        case Piece::WR: return lookup(rook_tbl, true);
        case Piece::BR: return lookup(rook_tbl, false);
        case Piece::WQ: return lookup(queen_tbl, true);
        case Piece::BQ: return lookup(queen_tbl, false);
        case Piece::WK: return lookup(king_mg, true);
        case Piece::BK: return lookup(king_mg, false);
        default: return 0;
    }
}

int evaluate_pawns(const Position& pos, Color c, Bitboard &passed, Bitboard &isolated, Bitboard &backward, Bitboard &candidate, Bitboard &connected) {
    int score=0;
    Bitboard pawns = (c==Color::White? pos.pieces(Piece::WP): pos.pieces(Piece::BP));
    Bitboard enemyPawns = (c==Color::White? pos.pieces(Piece::BP): pos.pieces(Piece::WP));

    int fileCount[8]={0};
    for (int sq=0;sq<64;++sq) if ( (pawns>>sq)&1 ) fileCount[file_of(sq)]++;

    passed=0; isolated=0; backward=0; candidate=0; connected=0;
    for (int sq=0;sq<64;++sq) if ((pawns>>sq)&1) {
        int f=file_of(sq), r=rank_of(sq);
        bool iso = true;
        if (f>0 && fileCount[f-1]>0) iso=false;
        if (f<7 && fileCount[f+1]>0) iso=false;
        if (iso) { isolated |= (1ULL<<sq); score -= 15; }

        if (fileCount[f]>1) score -= 12;

        // backward: pawn behind friendly pawns on adjacent files and cannot be defended
        bool back = false;
        if (f>0 && fileCount[f-1]==0 && f<7 && fileCount[f+1]==0) {
            // no friendly pawns on adjacent files ahead?
            bool aheadFriendly=false;
            int dir = (c==Color::White? 1:-1);
            for (int rr=r+dir; rr>=0 && rr<8; rr+=dir) {
                for (int df=-1;df<=1;++df) {
                    int nf=f+df; if (nf<0||nf>7) continue;
                    int tsq = square_of(nf,rr);
                    if ( (pawns>>tsq)&1 ) aheadFriendly=true;
                }
            }
            if (!aheadFriendly) { back=true; backward |= (1ULL<<sq); score -= 10; }
        }

        // connected: friendly pawn on adjacent file same rank or one behind
        for (int df=-1;df<=1;++df) if (df!=0) {
            int nf=f+df;
            if (nf<0||nf>7) continue;
            for (int dr=-1;dr<=1;++dr) {
                int nr=r+dr;
                if (nr<0||nr>=8) continue;
                int tsq=square_of(nf,nr);
                if ((pawns>>tsq)&1) { connected |= (1ULL<<sq); score += 5; goto next; }
            }
        }
        next:;

        // passed and candidate
        bool isPassed=true;
        bool isCandidate=true;
        int dir = (c==Color::White? 1:-1);
        int enemyAhead=0, friendlyAhead=0;
        for (int rr=r+dir; rr>=0 && rr<8; rr+=dir) {
            for (int df=-1;df<=1;++df) {
                int nf=f+df;
                if (nf<0||nf>7) continue;
                int tsq = square_of(nf,rr);
                if ( (enemyPawns>>tsq)&1 ) { isPassed=false; enemyAhead++; }
                if ( (pawns>>tsq)&1 ) friendlyAhead++;
            }
        }
        if (isPassed) { passed |= (1ULL<<sq); score += 20 + (c==Color::White? r*4 : (7-r)*4); }
        else if (enemyAhead==1 && friendlyAhead==0) { candidate |= (1ULL<<sq); score += 8; }
    }
    return score;
}

int evaluate_king_safety(const Position& pos, Color c) {
    int ks = pos.king_square(c);
    if (ks<0) return 0;
    int f=file_of(ks), r=rank_of(ks);
    int shield=0;
    int dir = (c==Color::White? 1:-1);
    for (int df=-1;df<=1;++df) for (int dr=1;dr<=2;++dr) {
        int nf=f+df, nr=r+dir*dr;
        if (nf<0||nf>7||nr<0||nr>=8) continue;
        int tsq=square_of(nf,nr);
        Piece p = pos.board()[tsq];
        if (p==(c==Color::White? Piece::WP: Piece::BP)) shield+=8;
    }
    int openPenalty=0;
    for (int df=-1;df<=1;++df) {
        int nf=f+df; if (nf<0||nf>7) continue;
        Bitboard fileBB=0;
        for (int rr=0;rr<8;++rr) fileBB |= 1ULL<<square_of(nf,rr);
        if ( (fileBB & (pos.pieces(Piece::WP)|pos.pieces(Piece::BP)))==0 ) openPenalty -= 12;
        else if ( (fileBB & (c==Color::White? pos.pieces(Piece::WP): pos.pieces(Piece::BP)))==0) openPenalty -=6;
    }
    return shield + openPenalty;
}

int evaluate_outposts(const Position& pos, Color c) {
    int score=0;
    Bitboard knights = (c==Color::White? pos.pieces(Piece::WN): pos.pieces(Piece::BN));
    Bitboard bishops = (c==Color::White? pos.pieces(Piece::WB): pos.pieces(Piece::BB));
    Bitboard enemyPawns = (c==Color::White? pos.pieces(Piece::BP): pos.pieces(Piece::WP));
    int rankMin = (c==Color::White? 3:2);
    int rankMax = (c==Color::White? 5:4);
    auto eval_piece_outpost = [&](Bitboard bb, int bonus){
        for (int sq=0;sq<64;++sq) if ((bb>>sq)&1) {
            int r=rank_of(sq), f=file_of(sq);
            if (r<rankMin||r>rankMax) continue;
            bool defendedByPawn=false;
            int pd = (c==Color::White? -1:1);
            for (int df:{-1,1}) {
                int nf=f+df, nr=r+pd;
                if (nf>=0&&nf<8&&nr>=0&&nr<8) {
                    int tsq=square_of(nf,nr);
                    Piece p=pos.board()[tsq];
                    if (p==(c==Color::White? Piece::WP: Piece::BP)) defendedByPawn=true;
                }
            }
            if (defendedByPawn) {
                bool attacked=false;
                int edir = (c==Color::White? 1:-1);
                for (int df:{-1,1}) {
                    int nf=f+df, nr=r+edir;
                    if (nf>=0&&nf<8&&nr>=0&&nr<8) {
                        int tsq=square_of(nf,nr);
                        if ((enemyPawns>>tsq)&1) attacked=true;
                    }
                }
                if (!attacked) score+=bonus;
            }
        }
    };
    eval_piece_outpost(knights, 25);
    eval_piece_outpost(bishops, 15);
    return score;
}

int evaluate_mobility(const Position& pos, Color c) {
    // Count pseudo-legal moves for side
    int mob=0;
    // Knight mobility
    Bitboard knights = (c==Color::White? pos.pieces(Piece::WN): pos.pieces(Piece::BN));
    for (int sq=0;sq<64;++sq) if ((knights>>sq)&1) {
        int f=file_of(sq), r=rank_of(sq);
        const int d[8][2]={{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
        for (auto &x : d) {
            int nf=f+x[0], nr=r+x[1];
            if (nf>=0&&nf<8&&nr>=0&&nr<8) {
                int to=square_of(nf,nr);
                Piece p=pos.board()[to];
                if (p==Piece::None || piece_color(p)!=c) mob+=2;
            }
        }
    }
    return mob;
}

int evaluate_space(const Position& pos, Color c) {
    // Reward genuinely controlled central squares, not merely empty squares.
    // Count only squares attacked by this side; squares also attacked by the
    // opponent receive a smaller bonus because they are contested.
    const Color them = (c == Color::White ? Color::Black : Color::White);
    int score = 0;
    for (int r = 2; r <= 5; ++r) {
        for (int f = 2; f <= 5; ++f) {
            const int sq = square_of(f, r);
            const bool ours = pos.square_attacked(sq, c);
            if (!ours) continue;
            const bool theirs = pos.square_attacked(sq, them);
            score += theirs ? 1 : 3;
        }
    }
    return score;
}

int evaluate_knight_discipline(const Position& pos, Color c) {
    // Opening: punish unsupported knight lunges (Ng5/Nb5) that SF19 keeps beating us with.
    if (pos.game_ply() > 24) return 0;
    int score = 0;
    Bitboard knights = (c == Color::White ? pos.pieces(Piece::WN) : pos.pieces(Piece::BN));
    Bitboard ourPawns = (c == Color::White ? pos.pieces(Piece::WP) : pos.pieces(Piece::BP));
    for (int sq = 0; sq < 64; ++sq) if ((knights >> sq) & 1) {
        int f = file_of(sq), r = rank_of(sq);
        int forward = (c == Color::White ? r : 7 - r);
        if (forward < 4) continue; // 5th rank and beyond
        bool pawnDef = false;
        int pd = (c == Color::White ? -1 : 1);
        for (int df : {-1, 1}) {
            int nf = f + df, nr = r + pd;
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) {
                if ((ourPawns >> square_of(nf, nr)) & 1) pawnDef = true;
            }
        }
        if (!pawnDef) {
            score -= 24;
            if (f == 1 || f == 6) score -= 16; // b/g files — the Ng5/Nb5 special
        }
    }
    return score;
}

int evaluate_threats(const Position& pos, Color c) {
    int score=0;
    Color them = (c==Color::White? Color::Black: Color::White);
    // Hanging pieces: enemy pieces attacked by own pawns but not defended
    for (int sq=0;sq<64;++sq) {
        Piece p = pos.board()[sq];
        if (p==Piece::None || piece_color(p)!=them) continue;
        if (pos.square_attacked(sq, c) && !pos.square_attacked(sq, them)) {
            // hanging
            score+=10;
        }
    }
    return score;
}

// NNUE is opt-in: a network is loaded only through the UCI NNUEFile option.
// This keeps classical-vs-NNUE tournaments honest and avoids silently loading
// an incompatible or stale network merely because a file exists on disk.
static NNUE g_nnue;
// Tunable share of NNUE in the blended evaluator. No effect until a network is loaded.
static int g_nnue_blend_percent = 70;

} // namespace

bool load_evaluation_nnue(const std::string& path) {
    if (path.empty() || !g_nnue.load(path)) return false;

    // A promoted network carries its tuned blend beside the weights. Candidate
    // validation uses /tmp and explicitly overrides NNUEBlend, while loading
    // networks/nnue.nnue restores the weight selected by the held-out match.
    const auto sep = path.find_last_of("/\\");
    const std::string directory = (sep == std::string::npos) ? "." : path.substr(0, sep);
    std::ifstream blend_file(directory + "/nnue_blend.txt");
    int stored_blend = 0;
    if (blend_file >> stored_blend) {
        g_nnue_blend_percent = std::clamp(stored_blend, 0, 100);
    }
    return true;
}

void set_evaluation_nnue_blend(int neural_percent) {
    g_nnue_blend_percent = std::clamp(neural_percent, 0, 100);
}

int get_evaluation_nnue_blend() {
    return g_nnue_blend_percent;
}

Score evaluate_handcrafted(const Position& pos) {
    Score score=0;
    const auto& b = pos.board();
    int white_bishops=0, black_bishops=0;
    Bitboard wPassed,bPassed,wIso,bIso,wBack,bBack,wCand,bCand,wConn,bConn;
    int wPawnStruct = evaluate_pawns(pos, Color::White, wPassed, wIso, wBack, wCand, wConn);
    int bPawnStruct = evaluate_pawns(pos, Color::Black, bPassed, bIso, bBack, bCand, bConn);
    score += wPawnStruct - bPawnStruct;

    score += evaluate_king_safety(pos, Color::White) - evaluate_king_safety(pos, Color::Black);
    score += evaluate_outposts(pos, Color::White) - evaluate_outposts(pos, Color::Black);
    score += evaluate_mobility(pos, Color::White) - evaluate_mobility(pos, Color::Black);
    score += evaluate_space(pos, Color::White) - evaluate_space(pos, Color::Black);
    score += evaluate_threats(pos, Color::White) - evaluate_threats(pos, Color::Black);
    score += evaluate_knight_discipline(pos, Color::White) - evaluate_knight_discipline(pos, Color::Black);

    for (int sq=0;sq<64;++sq) {
        Piece p=b[sq];
        if (p==Piece::None) continue;
        int v = value(p) + pst(p,sq);
        if (is_white(p)) score+=v; else score-=v;
        if (p==Piece::WB) ++white_bishops;
        if (p==Piece::BB) ++black_bishops;
    }
    if (white_bishops>=2) score+=32;
    if (black_bishops>=2) score-=32;

    for (int sq=0;sq<64;++sq) {
        Piece p=b[sq];
        if (p!=Piece::WR && p!=Piece::BR) continue;
        int f=file_of(sq);
        Bitboard fileBB=0;
        for(int r=0;r<8;++r) fileBB|=1ULL<<square_of(f,r);
        bool open = (fileBB & (pos.pieces(Piece::WP)|pos.pieces(Piece::BP)))==0;
        bool semi = (fileBB & (is_white(p)? pos.pieces(Piece::WP): pos.pieces(Piece::BP)))==0;
        int bonus = open? 18 : (semi? 10:0);
        if (is_white(p)) score+=bonus; else score-=bonus;
        int r=rank_of(sq);
        if ((is_white(p)&&r==6)||(!is_white(p)&&r==1)) {
            if (is_white(p)) score+=20; else score-=20;
        }
    }

    score += (pos.side_to_move()==Color::White? 8:-8);

    int totalMat=0;
    for(int sq=0;sq<64;++sq) if (b[sq]!=Piece::None) totalMat+=value(b[sq]);
    if (totalMat < 2500) {
        if (score>0) score = score * (120 + totalMat/20) / 100;
        else if (score<0) score = score * (120 + totalMat/20) / 100;
    }

    int side = (pos.side_to_move()==Color::White? 1:-1);
    Score final = score * side + 8;
    // Clamp to avoid fake mate scores
    final = std::clamp(final, -10000, 10000);
    return final;
}

Score evaluate(const Position& pos) {
    const Score classical = evaluate_handcrafted(pos);
    if (!g_nnue.is_loaded() || g_nnue_blend_percent <= 0) return classical;

    const Score neural = g_nnue.evaluate(pos);
    if (g_nnue_blend_percent >= 100) return std::clamp(neural, -10000, 10000);
    const int neural_weight = std::clamp(g_nnue_blend_percent, 0, 100);
    const Score blended =
        (neural * neural_weight + classical * (100 - neural_weight)) / 100;
    return std::clamp(blended, -10000, 10000);
}

} // namespace chess
