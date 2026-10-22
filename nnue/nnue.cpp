#include "nnue.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace chess {

int NNUE::make_feature(Color perspective, int kingSq, Piece p, int pieceSq) {
    // HalfKP matching training/train_nnue.py:
    // 10 types: own P,N,B,R,Q then enemy P,N,B,R,Q. Kings are not features.
    if (p == Piece::None || p == Piece::WK || p == Piece::BK) return -1;
    int king = (perspective == Color::White ? kingSq : kingSq ^ 56);
    int pcSq = (perspective == Color::White ? pieceSq : pieceSq ^ 56);
    bool own = (perspective == Color::White) ? is_white(p) : is_black(p);
    int pt = 0;
    switch (p) {
        case Piece::WP: case Piece::BP: pt = 0; break;
        case Piece::WN: case Piece::BN: pt = 1; break;
        case Piece::WB: case Piece::BB: pt = 2; break;
        case Piece::WR: case Piece::BR: pt = 3; break;
        case Piece::WQ: case Piece::BQ: pt = 4; break;
        default: return -1;
    }
    int type = (own ? 0 : 5) + pt;
    int bucket = king / 8;
    int feature = bucket * 640 + type * 64 + pcSq;
    if (feature < 0 || feature >= NNUE_FT_SIZE) return -1;
    return feature;
}

bool NNUE::load(const std::string& path) {
    if (path.empty()) { loaded_=false; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { loaded_=false; return false; }
    // Binary v2: 8-byte magic "NGNNUE02", then a little-endian uint32 version.
    // Reject old white-POV weights; tensor dimensions alone cannot distinguish semantics.
    char magic[8]{};
    std::uint32_t version = 0;
    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in || std::memcmp(magic, "NGNNUE02", sizeof(magic)) != 0 || version != 2) {
        loaded_ = false;
        feature_weights_.clear();
        feature_bias_.clear();
        l1_weights_.clear();
        l1_bias_.clear();
        l2_weights_.clear();
        l2_bias_ = 0;
        return false;
    }

    // Reject truncated and oversized files rather than accepting an incompatible prefix.
    try {
        feature_weights_.resize(NNUE_FT_SIZE * NNUE_HT1);
        feature_bias_.resize(NNUE_HT1);
        l1_weights_.resize(NNUE_HT1 * 2 * NNUE_HT2);
        l1_bias_.resize(NNUE_HT2);
        l2_weights_.resize(NNUE_HT2);

        in.read(reinterpret_cast<char*>(feature_weights_.data()),
                static_cast<std::streamsize>(feature_weights_.size() * sizeof(int16_t)));
        in.read(reinterpret_cast<char*>(feature_bias_.data()),
                static_cast<std::streamsize>(feature_bias_.size() * sizeof(int16_t)));
        in.read(reinterpret_cast<char*>(l1_weights_.data()),
                static_cast<std::streamsize>(l1_weights_.size() * sizeof(int16_t)));
        in.read(reinterpret_cast<char*>(l1_bias_.data()),
                static_cast<std::streamsize>(l1_bias_.size() * sizeof(int32_t)));
        in.read(reinterpret_cast<char*>(l2_weights_.data()),
                static_cast<std::streamsize>(l2_weights_.size() * sizeof(int16_t)));
        in.read(reinterpret_cast<char*>(&l2_bias_), sizeof(int32_t));

        const bool read_ok = static_cast<bool>(in) &&
                             in.gcount() == static_cast<std::streamsize>(sizeof(int32_t));
        const bool exact_size = in.peek() == std::char_traits<char>::eof();
        loaded_ = read_ok && exact_size;
    } catch (...) {
        loaded_ = false;
    }
    if (!loaded_) {
        feature_weights_.clear();
        feature_bias_.clear();
        l1_weights_.clear();
        l1_bias_.clear();
        l2_weights_.clear();
        l2_bias_=0;
        return false; // never pretend random weights are a trained net
    }
    return loaded_;
}

void NNUE::refresh_accumulator(const Position& pos, Accumulator& acc, Color perspective) const {
    if (!loaded_) { acc.white.fill(0); acc.black.fill(0); return; }
    std::array<int16_t, NNUE_HT1> &target = (perspective==Color::White? acc.white : acc.black);
    for(int i=0;i<NNUE_HT1;++i) target[i] = (i < (int)feature_bias_.size()? feature_bias_[i]:0); // start with bias
    int kingSq = pos.king_square(perspective);
    if (kingSq<0) return;
    const auto& board = pos.board();
    for (int sq=0;sq<64;++sq) {
        Piece p = board[sq];
        if (p==Piece::None) continue;
        int f = make_feature(perspective, kingSq, p, sq);
        // Kings are deliberately excluded from the trained feature map.
        // Never index feature_weights_ with the sentinel returned for them.
        if (f < 0) continue;
        const int offset = f * NNUE_HT1;
        for (int i = 0; i < NNUE_HT1; ++i) {
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
    // Network predicts side-to-move score with lanes ordered [us, them].
    const Color stm = pos.side_to_move();
    const auto &us = (stm == Color::White ? acc.white : acc.black);
    const auto &them = (stm == Color::White ? acc.black : acc.white);

    std::array<int32_t, NNUE_HT2> hidden{};
    for (int i = 0; i < NNUE_HT2; ++i) hidden[i] = l1_bias_[i];

    for (int j = 0; j < NNUE_HT1; ++j) {
        const int16_t value = std::clamp<int16_t>(us[j], 0, QA);
        for (int k = 0; k < NNUE_HT2; ++k) {
            hidden[k] += value * l1_weights_[j * NNUE_HT2 + k];
        }
    }
    for (int j = 0; j < NNUE_HT1; ++j) {
        const int16_t value = std::clamp<int16_t>(them[j], 0, QA);
        for (int k = 0; k < NNUE_HT2; ++k) {
            hidden[k] += value * l1_weights_[(NNUE_HT1 + j) * NNUE_HT2 + k];
        }
    }

    // Clipped ReLU 0..QB
    for (int k=0;k<NNUE_HT2;++k) {
        hidden[k] = std::clamp<int32_t>(hidden[k] / QA, 0, QB);
    }

    // The exported network uses QB-scaled hidden activations and QB-scaled
    // final weights, so both the bias and dot product are in QB*QB units.
    // Convert back to the trainer's normalized output, then to centipawns.
    std::int64_t out = l2_bias_;
    for (int k = 0; k < NNUE_HT2; ++k) {
        out += static_cast<std::int64_t>(hidden[k]) * l2_weights_[k];
    }
    const Score cp = static_cast<Score>(out * NNUE_SCALE / (QB * QB));
    return cp;
}

Score NNUE::evaluate(const Position& pos) const {
    if (!loaded_) return 0;
    Accumulator acc;
    refresh_accumulator(pos, acc, Color::White);
    refresh_accumulator(pos, acc, Color::Black);
    return evaluate(pos, acc);
}

} // namespace chess
