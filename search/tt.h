#pragma once
#include "../core/position.h"
#include <vector>
#include <array>
#include <cstdint>

namespace chess {

enum class TTFlag : std::uint8_t { Exact = 0, Alpha = 1, Beta = 2 };

struct TTEntry {
    Key key{0};
    Move best{};
    Score score{0};
    int16_t depth{-1};
    TTFlag flag{TTFlag::Exact};
    uint16_t gen{0};
    int16_t eval{0};
};

struct TTCluster {
    std::array<TTEntry, 4> entries{};
};

class TT {
public:
    explicit TT(std::size_t mb = 16) { resize_mb(mb); }
    void resize_mb(std::size_t mb);
    void clear();
    void new_search();
    TTEntry* probe(Key key);
    const TTEntry* probe(Key key) const;
    void store(Key key, int depth, Score score, TTFlag flag, Move best, Score eval, int ply);
    void prefetch(Key key) const;
    size_t hashfull() const;

private:
    TTEntry* replacement_slot(Key key);
    std::vector<TTCluster> table_;
    std::size_t mask_{0};
    uint16_t generation_{0};
};

} // namespace chess
