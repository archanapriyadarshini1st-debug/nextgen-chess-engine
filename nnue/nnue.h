#pragma once
#include "../core/position.h"
#include <string>
#include <array>

namespace chess {

constexpr int NNUE_FT_SIZE = 41024; // HalfKP 64*64*10? Simplified 41024
constexpr int NNUE_HT1 = 256;
constexpr int NNUE_HT2 = 32;
constexpr int NNUE_SCALE = 400;
constexpr int QA = 255;
constexpr int QB = 64;

class NNUE {
public:
    struct Accumulator {
        std::array<int16_t, NNUE_HT1> white{};
        std::array<int16_t, NNUE_HT1> black{};
        bool computed[2]{false,false};
    };

    bool load(const std::string& path);
    Score evaluate(const Position& pos) const;
    Score evaluate(const Position& pos, const Accumulator& acc) const;

    // incremental update
    void refresh_accumulator(const Position& pos, Accumulator& acc, Color perspective) const;
    void update_accumulator(const Position& pos, Accumulator& acc, Move m) const;

    bool is_loaded() const { return loaded_; }

private:
    bool loaded_ = false;
    // weights: FT -> L1
    std::vector<int16_t> feature_weights_; // [FT*HT1]
    std::vector<int16_t> feature_bias_; // HT1
    std::vector<int16_t> l1_weights_; // [2*HT1 * HT2] 512*32
    std::vector<int32_t> l1_bias_; // HT2
    std::vector<int16_t> l2_weights_; // HT2
    int32_t l2_bias_{0};

    // HalfKP feature indexing: king square + piece square * piece type
    static int make_feature(Color perspective, int kingSq, Piece p, int pieceSq);
};

} // namespace chess
