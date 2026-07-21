#include "eval.h"
#include "../nnue/nnue.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <iostream>

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
    int f = file_of(sq), r = rank_of(sq);
    int center = 14 - (std::abs(3 - f) + std::abs(3 - r))*2;
    switch (p) {
        case Piece::WP: return r * 9 + center/2;
        case Piece::BP: return (7 - r) * 9 + center/2;
        case Piece::WN: case Piece::BN: return center * 3;
        case Piece::WB: case Piece::BB: return center * 2 + (f==0||f==7? -2:0);
        case Piece::WR: case Piece::BR: return (f == 0 || f == 7) ? 8 : center;
        case Piece::WQ: case Piece::BQ: return center;
        case Piece::WK: return -r * 5 + center/3;
        case Piece::BK: return -(7 - r) * 5 + center/3;
        default: return 0;
    }
}

int evaluate_pawns(const Position& pos, Color c, Bitboard &passed, Bitboard &isolated) {
    int score=0;
    Bitboard pawns = (c==Color::White? pos.pieces(Piece::WP): pos.pieces(Piece::BP));
    Bitboard enemyPawns = (c==Color::White? pos.pieces(Piece::BP): pos.pieces(Piece::WP));

    int fileCount[8]={0};
    for (int sq=0;sq<64;++sq) if ( (pawns>>sq)&1 ) fileCount[file_of(sq)]++;

    passed=0; isolated=0;
    for (int sq=0;sq<64;++sq) if ((pawns>>sq)&1) {
        int f=file_of(sq), r=rank_of(sq);
        bool iso = true;
        if (f>0 && fileCount[f-1]>0) iso=false;
        if (f<7 && fileCount[f+1]>0) iso=false;
        if (iso) { isolated |= (1ULL<<sq); score -= 15; }
        if (fileCount[f]>1) score -= 12;
        bool isPassed=true;
        int dir = (c==Color::White? 1:-1);
        for (int rr=r+dir; rr>=0 && rr<8; rr+=dir) {
            for (int df=-1;df<=1;++df) {
                int nf=f+df;
                if (nf<0||nf>7) continue;
                int tsq = square_of(nf,rr);
                if ( (enemyPawns>>tsq)&1 ) isPassed=false;
            }
        }
        if (isPassed) { passed |= (1ULL<<sq); score += 20 + (c==Color::White? r*4 : (7-r)*4); }
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
    Bitboard enemyPawns = (c==Color::White? pos.pieces(Piece::BP): pos.pieces(Piece::WP));
    int rankMin = (c==Color::White? 3:2);
    int rankMax = (c==Color::White? 5:4);
    for (int sq=0;sq<64;++sq) if ((knights>>sq)&1) {
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
            if (!attacked) score+=25;
        }
    }
    return score;
}

// Global NNUE with thread-safe lazy load
static NNUE g_nnue;
static bool g_nnue_tried=false;
static std::once_flag g_nnue_flag;

void try_load_nnue() {
    std::call_once(g_nnue_flag, [](){
        // Try common paths
        const char* paths[] = {"networks/nnue.nnue","/home/user/nextgen-chess-engine/networks/nnue.nnue","nnue.nnue","/tmp/nnue.nnue"};
        for(auto p: paths){
            if (g_nnue.load(p)) {
                std::cout << "info string NNUE loaded from " << p << "\n";
                break;
            }
        }
        g_nnue_tried=true;
    });
}

} // namespace

Score evaluate_handcrafted(const Position& pos) {
    Score score=0;
    const auto& b = pos.board();
    int white_bishops=0, black_bishops=0;
    Bitboard wPassed,bPassed,wIso,bIso;
    int wPawnStruct = evaluate_pawns(pos, Color::White, wPassed, wIso);
    int bPawnStruct = evaluate_pawns(pos, Color::Black, bPassed, bIso);
    score += wPawnStruct - bPawnStruct;

    score += evaluate_king_safety(pos, Color::White) - evaluate_king_safety(pos, Color::Black);
    score += evaluate_outposts(pos, Color::White) - evaluate_outposts(pos, Color::Black);

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
    return score * side + 8;
}

Score evaluate(const Position& pos) {
    // Try to use NNUE if available, else handcrafted
    try_load_nnue();
    if (g_nnue.is_loaded()) {
        // Use NNUE with incremental accumulator
        Score nnueScore = g_nnue.evaluate(pos);
        // Hybrid: 70% NNUE + 30% handcrafted for stability during early training
        Score classical = evaluate_handcrafted(pos);
        return (nnueScore*7 + classical*3)/10;
    }
    return evaluate_handcrafted(pos);
}

} // namespace chess
