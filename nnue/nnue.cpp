#include "nnue.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

namespace chess {

int NNUE::make_feature(Color perspective, int kingSq, Piece p, int pieceSq) {
    const int pi = piece_index(p);
    if (pi < 0) return -1;
    const int type = pi % 6;              // 0=P 1=N 2=B 3=R 4=Q 5=K
    if (type == 5) return -1;             // kings are not features

    const bool own = (piece_color(p) == perspective);
    const int t = (own ? 0 : 5) + type;   // 0..9, perspective relative

    const int ksq = (perspective == Color::White) ? kingSq  : (kingSq  ^ 56);
    const int psq = (perspective == Color::White) ? pieceSq : (pieceSq ^ 56);

    return (ksq / 8) * (NNUE_PIECE_TYPES * 64) + t * 64 + psq;
}

bool NNUE::load(const std::string& path) {
    loaded_ = false;
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    NNUEHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in || std::strncmp(h.magic, "NGCEv3", 6) != 0) {
        std::cout << "info string NNUE " << path << ": bad magic, ignoring\n";
        return false;
    }
    if (h.ft != NNUE_FT_SIZE || h.ht1 != NNUE_HT1 || h.ht2 != NNUE_HT2
        || h.qa != QA || h.qb != QB || h.stm_relative != 1u
        || h.threat_inputs != NNUE_THREAT_INPUTS) {
        std::cout << "info string NNUE " << path << ": dimension/convention mismatch"
                  << " (ft=" << h.ft << " ht1=" << h.ht1 << " ht2=" << h.ht2
                  << " stm_relative=" << h.stm_relative
                  << " threat_inputs=" << h.threat_inputs << "), ignoring\n";
        return false;
    }

    feature_weights_.resize(static_cast<std::size_t>(NNUE_FT_SIZE) * NNUE_HT1);
    feature_bias_.resize(NNUE_HT1);
    l1_weights_.resize(static_cast<std::size_t>(NNUE_HT1 * 2 + NNUE_THREAT_INPUTS) * NNUE_HT2);
    l1_bias_.resize(NNUE_HT2);
    l2_weights_.resize(NNUE_HT2);

    auto rd = [&](void* p, std::size_t bytes) { in.read(reinterpret_cast<char*>(p), bytes); return static_cast<bool>(in); };
    bool ok = rd(feature_weights_.data(), feature_weights_.size() * sizeof(int16_t))
           && rd(feature_bias_.data(),    feature_bias_.size()    * sizeof(int16_t))
           && rd(l1_weights_.data(),      l1_weights_.size()      * sizeof(int16_t))
           && rd(l1_bias_.data(),         l1_bias_.size()         * sizeof(int32_t))
           && rd(l2_weights_.data(),      l2_weights_.size()      * sizeof(int16_t))
           && rd(&l2_bias_,               sizeof(int32_t));

    if (!ok) {
        std::cout << "info string NNUE " << path << ": truncated file, ignoring\n";
        feature_weights_.clear();
        return false;
    }
    loaded_ = true;
    std::cout << "info string NNUE loaded " << path << "\n";
    return true;
}

void NNUE::refresh_accumulator(const Position& pos, Accumulator& acc, Color perspective) const {
    auto& target = (perspective == Color::White) ? acc.white : acc.black;
    target.fill(0);
    if (!loaded_) return;
    for (int i = 0; i < NNUE_HT1; ++i) target[i] = feature_bias_[i];

    const int kingSq = pos.king_square(perspective);
    if (kingSq < 0) return;

    const auto& board = pos.board();
    for (int sq = 0; sq < 64; ++sq) {
        const Piece p = board[sq];
        if (p == Piece::None) continue;
        const int f = make_feature(perspective, kingSq, p, sq);
        if (f < 0) continue;
        const int16_t* w = &feature_weights_[static_cast<std::size_t>(f) * NNUE_HT1];
        for (int i = 0; i < NNUE_HT1; ++i) target[i] += w[i];
    }
    acc.computed[perspective == Color::White ? 0 : 1] = true;
}

void NNUE::update_accumulator(const Position& pos, Accumulator& acc, Move m) const {
    if (!loaded_) return;

    const Piece moving = pos.piece_at(m.from());
    if (moving == Piece::None) return;
    const Color us = piece_color(moving);
    const Piece captured = (m.flags() & FLAG_EN_PASSANT)
        ? (us == Color::White ? Piece::BP : Piece::WP)
        : pos.piece_at(m.to());
    const int capture_sq = (m.flags() & FLAG_EN_PASSANT)
        ? (us == Color::White ? m.to() - 8 : m.to() + 8)
        : m.to();

    auto delta = [&](Color perspective, Piece piece, int sq, int sign) {
        if (piece == Piece::None || sq < 0 || sq >= 64) return;
        const int king = pos.king_square(perspective);
        const int f = make_feature(perspective, king, piece, sq);
        if (f < 0) return;
        auto& a = perspective == Color::White ? acc.white : acc.black;
        const int16_t* w = &feature_weights_[static_cast<std::size_t>(f) * NNUE_HT1];
        for (int i = 0; i < NNUE_HT1; ++i) a[i] += sign * w[i];
    };

    // A king move changes the HalfKP bucket, so that perspective needs a
    // rebuild. The other perspective still gets cheap piece deltas.
    const bool king_move = moving == Piece::WK || moving == Piece::BK;
    if (king_move) acc.computed[static_cast<int>(us)] = false;

    for (int side = 0; side < 2; ++side) {
        const Color perspective = static_cast<Color>(side);
        if (!acc.computed[side]) continue;
        if (!king_move || perspective != us) {
            // The source piece is always removed; promotion then adds the new piece.
            delta(perspective, moving, m.from(), -1);
            if (captured != Piece::None) delta(perspective, captured, capture_sq, -1);
            if (m.flags() & FLAG_PROMOTION) {
                const Piece promo = make_piece(us, m.promo() == 1 ? 2 : m.promo() == 2 ? 3 : m.promo() == 3 ? 4 : 5);
                delta(perspective, promo, m.to(), +1);
            } else {
                delta(perspective, moving, m.to(), +1);
            }

            // Castling moves the rook as well.
            if (m.flags() & FLAG_KING_CASTLE) {
                const int rook_from = us == Color::White ? 7 : 63;
                const int rook_to   = us == Color::White ? 5 : 61;
                delta(perspective, make_piece(us, 4), rook_from, -1);
                delta(perspective, make_piece(us, 4), rook_to, +1);
            } else if (m.flags() & FLAG_QUEEN_CASTLE) {
                const int rook_from = us == Color::White ? 0 : 56;
                const int rook_to   = us == Color::White ? 3 : 59;
                delta(perspective, make_piece(us, 4), rook_from, -1);
                delta(perspective, make_piece(us, 4), rook_to, +1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SIGN CONVENTION
// The accumulators are concatenated (side-to-move, other-side) and the network
// is trained on side-to-move-relative labels, so the raw output IS already
// stm-relative. The old code concatenated (white, black) but then flipped the
// sign for black - so for half of all positions the net was fitted against a
// sign-flipped target, and the blend with the (stm-relative) handcrafted eval
// pulled in opposite directions.
// ---------------------------------------------------------------------------
Score NNUE::evaluate(const Position& pos, const Accumulator& acc) const {
    if (!loaded_) return 0;

    const bool whiteToMove = pos.side_to_move() == Color::White;
    const auto& us   = whiteToMove ? acc.white : acc.black;
    const auto& them = whiteToMove ? acc.black : acc.white;

    std::array<int32_t, NNUE_HT2> hidden{};
    for (int k = 0; k < NNUE_HT2; ++k) hidden[k] = l1_bias_[k];

    for (int j = 0; j < NNUE_HT1; ++j) {
        const int32_t a = std::clamp<int32_t>(us[j], 0, QA);
        if (!a) continue;
        const int16_t* w = &l1_weights_[static_cast<std::size_t>(j) * NNUE_HT2];
        for (int k = 0; k < NNUE_HT2; ++k) hidden[k] += a * w[k];
    }
    for (int j = 0; j < NNUE_HT1; ++j) {
        const int32_t a = std::clamp<int32_t>(them[j], 0, QA);
        if (!a) continue;
        const int16_t* w = &l1_weights_[static_cast<std::size_t>(NNUE_HT1 + j) * NNUE_HT2];
        for (int k = 0; k < NNUE_HT2; ++k) hidden[k] += a * w[k];
    }

    // Two side-relative threat inputs: enemy non-king pieces attacked by us,
    // and our non-king pieces attacked by the opponent.
    int enemy_threats = 0, own_threats = 0;
    const Color usColor = whiteToMove ? Color::White : Color::Black;
    const Color themColor = opposite(usColor);
    for (int sq = 0; sq < 64; ++sq) {
        const Piece pc = pos.piece_at(sq);
        if (pc == Piece::None || pc == Piece::WK || pc == Piece::BK) continue;
        if (piece_color(pc) == themColor && pos.square_attacked(sq, usColor)) ++enemy_threats;
        if (piece_color(pc) == usColor && pos.square_attacked(sq, themColor)) ++own_threats;
    }
    const int threat_vals[NNUE_THREAT_INPUTS] = {
        std::min(QA, enemy_threats * QA / 8),
        std::min(QA, own_threats * QA / 8)
    };
    for (int j = 0; j < NNUE_THREAT_INPUTS; ++j) {
        const int16_t* w = &l1_weights_[static_cast<std::size_t>(NNUE_HT1 * 2 + j) * NNUE_HT2];
        for (int k = 0; k < NNUE_HT2; ++k) hidden[k] += threat_vals[j] * w[k];
    }

    for (int k = 0; k < NNUE_HT2; ++k) hidden[k] = std::clamp<int32_t>(hidden[k] / QA, 0, QB);

    int64_t out = l2_bias_;
    for (int k = 0; k < NNUE_HT2; ++k) out += static_cast<int64_t>(hidden[k]) * l2_weights_[k];
    out /= QB;
    out = out * NNUE_SCALE / (QA * QB / 2);

    return static_cast<Score>(std::clamp<int64_t>(out, -10000, 10000));   // no sign flip
}

Score NNUE::evaluate(const Position& pos) const {
    if (!loaded_) return 0;
    Accumulator acc;
    refresh_accumulator(pos, acc, Color::White);
    refresh_accumulator(pos, acc, Color::Black);
    return evaluate(pos, acc);
}

} // namespace chess
