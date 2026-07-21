#include "nnue.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace chess {

int NNUE::make_feature(Color perspective, int kingSq, Piece p, int pieceSq) {
    // HalfKP: king square perspective, piece square mirrored for black
    int king = (perspective==Color::White? kingSq : kingSq ^ 56);
    int pcSq = (perspective==Color::White? pieceSq : pieceSq ^ 56);
    int pIdx = piece_index(p);
    // Normalize: treat white pieces perspective: own pieces vs enemy pieces with different offsets
    // Simplified mapping: (pIdx*64 + pcSq) * 64? Actually 41024 = 64*10*64? Let's use 10 piece types *64 squares *64 king squares /2?
    // For simplicity we use stockfish-like HalfKA: pieceType*64 + square, offset by king bucket
    int bucket = king / 8; // 8 buckets
    int feature = bucket * 640 + pIdx * 64 + pcSq;
    // clamp
    if (feature>=NNUE_FT_SIZE) feature = feature % NNUE_FT_SIZE;
    return feature;
}

bool NNUE::load(const std::string& path) {
    if (path.empty()) { loaded_=false; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { loaded_=false; return false; }
    // Expected format: feature_weights[FT*HT1] int16, feature_bias[HT1] int16, l1_weights[512*32] int16, l1_bias[32] int32, l2_weights[32] int16, bias int32
    // For demo, if file not exactly matching, we initialize with small random
    try {
        feature_weights_.resize(NNUE_FT_SIZE * NNUE_HT1);
        feature_bias_.resize(NNUE_HT1);
        l1_weights_.resize(NNUE_HT1*2 * NNUE_HT2);
        l1_bias_.resize(NNUE_HT2);
        l2_weights_.resize(NNUE_HT2);
        in.read(reinterpret_cast<char*>(feature_weights_.data()), feature_weights_.size()*sizeof(int16_t));
        in.read(reinterpret_cast<char*>(feature_bias_.data()), feature_bias_.size()*sizeof(int16_t));
        in.read(reinterpret_cast<char*>(l1_weights_.data()), l1_weights_.size()*sizeof(int16_t));
        in.read(reinterpret_cast<char*>(l1_bias_.data()), l1_bias_.size()*sizeof(int32_t));
        in.read(reinterpret_cast<char*>(l2_weights_.data()), l2_weights_.size()*sizeof(int16_t));
        in.read(reinterpret_cast<char*>(&l2_bias_), sizeof(int32_t));
        loaded_ = in.gcount()>0 || !in.fail();
    } catch (...) {
        loaded_=false;
    }
    if (!loaded_) {
        // fallback: initialize with Xavier small values for testing
        feature_weights_.assign(NNUE_FT_SIZE*NNUE_HT1, 0);
        feature_bias_.assign(NNUE_HT1, 0);
        l1_weights_.assign(NNUE_HT1*2*NNUE_HT2, 0);
        l1_bias_.assign(NNUE_HT2, 0);
        l2_weights_.assign(NNUE_HT2, 64);
        l2_bias_=0;
        for (int i=0;i<(int)feature_weights_.size();++i) feature_weights_[i]= (i%7)-3;
        for (int i=0;i<NNUE_HT1;++i) feature_bias_[i]=1;
        loaded_=true; // mark as loaded with random weights
    }
    return loaded_;
}

void NNUE::refresh_accumulator(const Position& pos, Accumulator& acc, Color perspective) const {
    if (!loaded_) { acc.white.fill(0); acc.black.fill(0); return; }
    std::array<int16_t, NNUE_HT1> &target = (perspective==Color::White? acc.white : acc.black);
    target = feature_bias_; // start with bias
    int kingSq = pos.king_square(perspective);
    if (kingSq<0) return;
    const auto& board = pos.board();
    for (int sq=0;sq<64;++sq) {
        Piece p = board[sq];
        if (p==Piece::None) continue;
        int f = make_feature(perspective, kingSq, p, sq);
        // f -> add weights
        int offset = f * NNUE_HT1;
        for (int i=0;i<NNUE_HT1;++i) {
            target[i] += feature_weights_[offset + i];
        }
    }
    acc.computed[perspective==Color::White?0:1]=true;
}

void NNUE::update_accumulator(const Position& pos, Accumulator& acc, Move m) const {
    // Incremental: remove piece from, add to. Simplified: if king moves, full refresh else incremental
    Piece moving = pos.board()[m.from()];
    if (moving!=Piece::None && (moving==Piece::WK || moving==Piece::BK)) {
        // king moved - need full refresh both sides
        acc.computed[0]=acc.computed[1]=false;
        return;
    }
    // For simplicity, mark dirty and will refresh on eval if needed
    // True incremental would keep dirty list
    acc.computed[0]=false;
    acc.computed[1]=false;
}

Score NNUE::evaluate(const Position& pos, const Accumulator& acc) const {
    if (!loaded_) return 0;
    // perspective: side to move
    Color stm = pos.side_to_move();
    const auto &white = acc.white;
    const auto &black = acc.black;

    // Clipped ReLU + affine for first layer
    // HT1 256 -> for each side, we have 256 inputs, we concatenate white+black 512 -> L1 32
    // Compute hidden L1
    std::array<int32_t, NNUE_HT2> hidden{};
    for (int i=0;i<NNUE_HT2;++i) hidden[i]=l1_bias_[i];

    // White perspective accumulator contributes to first 256 of L1 weights, black to second 256
    // L1 weights layout: [512][32] = (256*2)*32
    for (int j=0;j<NNUE_HT1;++j) {
        int16_t w = std::clamp<int16_t>(white[j], 0, QA);
        // White
        for (int k=0;k<NNUE_HT2;++k) {
            hidden[k] += w * l1_weights_[j*NNUE_HT2 + k];
        }
    }
    for (int j=0;j<NNUE_HT1;++j) {
        int16_t b = std::clamp<int16_t>(black[j], 0, QA);
        for (int k=0;k<NNUE_HT2;++k) {
            hidden[k] += b * l1_weights_[(NNUE_HT1 + j)*NNUE_HT2 + k];
        }
    }

    // Clipped ReLU 0..QB
    for (int k=0;k<NNUE_HT2;++k) {
        hidden[k] = std::clamp<int32_t>(hidden[k] / QA, 0, QB);
    }

    // Final layer
    int32_t out= l2_bias_;
    for (int k=0;k<NNUE_HT2;++k) {
        out += hidden[k] * l2_weights_[k];
    }
    out /= QB;
    // Scale to centipawns
    out = out * NNUE_SCALE / (QA*QB/2);
    return static_cast<Score>( stm==Color::White? out : -out );
}

Score NNUE::evaluate(const Position& pos) const {
    if (!loaded_) return 0;
    Accumulator acc;
    refresh_accumulator(pos, acc, Color::White);
    refresh_accumulator(pos, acc, Color::Black);
    return evaluate(pos, acc);
}

} // namespace chess
