
#include "tt.h"
#include <algorithm>
#include <bit>

namespace chess {
void TT::resize_mb(std::size_t mb) {
    std::size_t bytes = std::max<std::size_t>(1, mb) * 1024ull * 1024ull;
    std::size_t entries = std::max<std::size_t>(1, bytes / sizeof(TTEntry));
    entries = std::bit_ceil(entries);
    table_.assign(entries, TTEntry{});
    mask_ = entries - 1;
}

TTEntry* TT::probe(Key key) {
    TTEntry& e = table_[key & mask_];
    return e.key == key ? &e : nullptr;
}

const TTEntry* TT::probe(Key key) const {
    const TTEntry& e = table_[key & mask_];
    return e.key == key ? &e : nullptr;
}

void TT::store(Key key, int depth, Score score, TTFlag flag, Move best) {
    TTEntry& e = table_[key & mask_];
    if (e.key != key || depth >= e.depth) {
        e.key = key;
        e.depth = static_cast<int16_t>(depth);
        e.score = score;
        e.flag = flag;
        e.best = best;
    }
}

} // namespace chess
