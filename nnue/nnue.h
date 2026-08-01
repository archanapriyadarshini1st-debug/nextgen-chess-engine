#pragma once
#include "../core/position.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {

// ---------------------------------------------------------------------------
// HalfKP-lite:  8 king buckets x 10 perspective-relative piece types x 64 squares
// The old FT_SIZE of 41024 only ever addressed 5120 of those slots (and kings
// were accidentally folded in, overflowing into the next bucket).
// ---------------------------------------------------------------------------
constexpr int NNUE_KING_BUCKETS = 8;
constexpr int NNUE_PIECE_TYPES  = 10;
constexpr int NNUE_FT_SIZE      = NNUE_KING_BUCKETS * NNUE_PIECE_TYPES * 64; // 5120
constexpr int NNUE_HT1   = 256;
constexpr int NNUE_HT2   = 32;
constexpr int NNUE_THREAT_INPUTS = 2;
constexpr int NNUE_SCALE = 400;
constexpr int QA = 255;
constexpr int QB = 64;

// Written by training/train_nnue.py, validated on load. Guarantees the trainer
// and the engine agree on dimensions AND on the accumulator ordering.
#pragma pack(push, 1)
struct NNUEHeader {
    char     magic[8];      // "NGCEv3\0\0"
    uint32_t ft, ht1, ht2;
    uint32_t qa, qb;
    uint32_t stm_relative;  // 1 = accumulators are ordered (side-to-move, other)
    uint32_t threat_inputs;  // NNUE_THREAT_INPUTS
};
#pragma pack(pop)

class NNUE {
public:
    struct Accumulator {
        std::array<int32_t, NNUE_HT1> white{};
        std::array<int32_t, NNUE_HT1> black{};
        bool computed[2]{false, false};
    };

    bool load(const std::string& path);
    Score evaluate(const Position& pos) const;
    Score evaluate(const Position& pos, const Accumulator& acc) const;

    void refresh_accumulator(const Position& pos, Accumulator& acc, Color perspective) const;
    void update_accumulator(const Position& pos, Accumulator& acc, Move m) const;

    bool is_loaded() const { return loaded_; }

    // -1 for kings (excluded from the feature set), else 0..NNUE_FT_SIZE-1
    static int make_feature(Color perspective, int kingSq, Piece p, int pieceSq);

private:
    bool loaded_ = false;
    std::vector<int16_t> feature_weights_;
    std::vector<int16_t> feature_bias_;
    std::vector<int16_t> l1_weights_;
    std::vector<int32_t> l1_bias_;
    std::vector<int16_t> l2_weights_;
    int32_t l2_bias_{0};
};

} // namespace chess
