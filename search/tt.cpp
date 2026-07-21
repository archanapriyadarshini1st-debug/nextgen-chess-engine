#include "tt.h"
#include <algorithm>
#include <bit>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace chess {

void TT::resize_mb(std::size_t mb) {
    std::size_t bytes = std::max<std::size_t>(1, mb) * 1024ull * 1024ull;
    std::size_t clusters = std::max<std::size_t>(1, bytes / sizeof(TTCluster));
    clusters = std::bit_floor(clusters);
    if (clusters==0) clusters=1;
    table_.assign(clusters, TTCluster{});
    mask_ = clusters - 1;
    generation_ = 0;
}

void TT::clear() {
    for (auto &c : table_) for (auto &e : c.entries) e = TTEntry{};
    generation_=0;
}

void TT::new_search() { generation_ += 8; }

void TT::prefetch(Key key) const {
#if defined(__x86_64__)
    _mm_prefetch((const char*)&table_[key & mask_], _MM_HINT_T0);
#endif
}

TTEntry* TT::probe(Key key) {
    TTCluster &cluster = table_[key & mask_];
    for (int i=0;i<4;++i) {
        if (cluster.entries[i].key == key) return &cluster.entries[i];
    }
    // return best replace candidate (lowest depth, oldest gen)
    TTEntry* replace = &cluster.entries[0];
    for (int i=1;i<4;++i) {
        if (cluster.entries[i].gen != generation_) { replace=&cluster.entries[i]; break; }
        if (cluster.entries[i].depth < replace->depth) replace=&cluster.entries[i];
    }
    return replace;
}

const TTEntry* TT::probe(Key key) const {
    const TTCluster &cluster = table_[key & mask_];
    for (int i=0;i<4;++i) {
        if (cluster.entries[i].key == key) return &cluster.entries[i];
    }
    return nullptr;
}

void TT::store(Key key, int depth, Score score, TTFlag flag, Move best, Score eval, int ply) {
    TTEntry* e = probe(key);
    // allow overwrite if deeper or exact or gen mismatch
    bool isNew = e->key != key;
    if (!isNew && e->depth > depth + 2 && e->gen==generation_ && flag!=TTFlag::Exact) return;
    if (isNew || depth+2 >= e->depth || flag==TTFlag::Exact) {
        if (best.is_null() && e->key==key) best = e->best;
        e->key = key;
        // mate distance handling
        Score v = score;
        if (v >  MATE_SCORE-1000) v += ply;
        else if (v < -MATE_SCORE+1000) v -= ply;
        e->score = v;
        e->eval = eval;
        e->depth = static_cast<int16_t>(depth);
        e->flag = flag;
        e->best = best;
        e->gen = generation_;
    }
}

size_t TT::hashfull() const {
    size_t cnt=0;
    size_t sample = std::min<size_t>(1000, table_.size());
    for (size_t i=0;i<sample;++i) {
        for (int j=0;j<4;++j) if (table_[i].entries[j].key!=0 && table_[i].entries[j].gen==generation_) cnt++;
    }
    return cnt * 1000 / (sample*4);
}

} // namespace chess
