#include "tt.h"
#include "../core/bitboard.h"
#include <algorithm>
#include <bit>

namespace chess {

namespace {
constexpr int   MOVE_BITS  = 21;
constexpr std::uint64_t MOVE_MASK = (1ULL << MOVE_BITS) - 1;
constexpr int   SCORE_SHIFT = 21;
constexpr int   EVAL_SHIFT  = 37;
constexpr int   DEPTH_SHIFT = 53;   // 7 bits, stores depth+1
constexpr int   FLAG_SHIFT  = 60;   // 2 bits
constexpr int   GEN_SHIFT   = 62;   // 2 bits
inline std::uint64_t enc16(int v) { return static_cast<std::uint64_t>(v + 32768) & 0xFFFFULL; }
inline int          dec16(std::uint64_t v) { return static_cast<int>(v & 0xFFFFULL) - 32768; }
}

std::uint64_t TT::pack(Move best, Score score, Score eval, int depth, TTFlag flag, unsigned gen) {
    depth = std::clamp(depth, -1, 125);
    score = std::clamp<Score>(score, -32000, 32000);
    eval  = std::clamp<Score>(eval,  -32000, 32000);
    return (static_cast<std::uint64_t>(best.raw) & MOVE_MASK)
         | (enc16(score) << SCORE_SHIFT)
         | (enc16(eval)  << EVAL_SHIFT)
         | (static_cast<std::uint64_t>(depth + 1) << DEPTH_SHIFT)
         | (static_cast<std::uint64_t>(static_cast<unsigned>(flag) & 3u) << FLAG_SHIFT)
         | (static_cast<std::uint64_t>(gen & 3u) << GEN_SHIFT);
}

void TT::unpack(std::uint64_t d, TTData& out, unsigned& gen) {
    out.best  = Move(static_cast<std::uint32_t>(d & MOVE_MASK));
    out.score = static_cast<Score>(dec16(d >> SCORE_SHIFT));
    out.eval  = static_cast<Score>(dec16(d >> EVAL_SHIFT));
    out.depth = static_cast<int>((d >> DEPTH_SHIFT) & 0x7FULL) - 1;
    out.flag  = static_cast<TTFlag>((d >> FLAG_SHIFT) & 3ULL);
    gen       = static_cast<unsigned>((d >> GEN_SHIFT) & 3ULL);
}

void TT::resize_mb(std::size_t mb) {
    std::size_t bytes    = std::max<std::size_t>(1, mb) * 1024ULL * 1024ULL;
    std::size_t clusters = std::max<std::size_t>(1, bytes / sizeof(TTCluster));
    clusters = std::bit_floor(clusters);
    table_ = std::vector<TTCluster>(clusters);
    mask_ = clusters - 1;
    generation_.store(0, std::memory_order_relaxed);
}

void TT::clear() {
    for (auto& c : table_)
        for (auto& e : c.entries) {
            e.key.store(0, std::memory_order_relaxed);
            e.data.store(0, std::memory_order_relaxed);
        }
    generation_.store(0, std::memory_order_relaxed);
}

void TT::new_search() { generation_.fetch_add(1, std::memory_order_relaxed); }

void TT::prefetch(Key key) const {
    ::Bitboard::prefetch(const_cast<void*>(static_cast<const void*>(&table_[key & mask_])));
}

TTData TT::probe(Key key) const {
    const TTCluster& c = table_[key & mask_];
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t k = c.entries[i].key.load(std::memory_order_relaxed);
        const std::uint64_t d = c.entries[i].data.load(std::memory_order_relaxed);
        if (d && (k ^ d) == key) {
            TTData out; unsigned g;
            unpack(d, out, g);
            out.hit = true;
            return out;
        }
    }
    return TTData{};
}

void TT::store(Key key, int depth, Score score, TTFlag flag, Move best, Score eval) {
    TTCluster& c = table_[key & mask_];
    const unsigned gen = generation_.load(std::memory_order_relaxed) & 3u;

    TTEntry* replace = &c.entries[0];
    int worst = 1 << 30;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t k = c.entries[i].key.load(std::memory_order_relaxed);
        const std::uint64_t d = c.entries[i].data.load(std::memory_order_relaxed);
        if (!d) { replace = &c.entries[i]; worst = -(1 << 30); break; }
        if ((k ^ d) == key) {
            TTData old; unsigned og; unpack(d, old, og);
            // keep the deeper result unless this is an exact score or a re-search
            if (old.depth > depth + 3 && og == gen && flag != TTFlag::Exact) return;
            if (best.is_null()) best = old.best;
            replace = &c.entries[i];
            worst = -(1 << 30);
            break;
        }
        TTData old; unsigned og; unpack(d, old, og);
        const int age = static_cast<int>((gen + 4 - og) & 3u);
        const int value = old.depth - age * 8;
        if (value < worst) { worst = value; replace = &c.entries[i]; }
    }

    const std::uint64_t data = pack(best, score, eval, depth, flag, gen);
    replace->data.store(data, std::memory_order_relaxed);
    replace->key.store(key ^ data, std::memory_order_relaxed);
}

std::size_t TT::hashfull() const {
    std::size_t cnt = 0;
    const std::size_t sample = std::min<std::size_t>(1000, table_.size());
    const unsigned gen = generation_.load(std::memory_order_relaxed) & 3u;
    for (std::size_t i = 0; i < sample; ++i)
        for (int j = 0; j < 4; ++j) {
            const std::uint64_t d = table_[i].entries[j].data.load(std::memory_order_relaxed);
            if (!d) continue;
            TTData o; unsigned g; unpack(d, o, g);
            if (g == gen) ++cnt;
        }
    return sample ? cnt * 1000 / (sample * 4) : 0;
}

} // namespace chess
