
#pragma once
#include "../core/position.h"
#include <vector>

namespace chess {

enum class TTFlag : std::uint8_t { Exact = 0, Alpha = 1, Beta = 2 };

struct TTEntry {
    Key key{0};
    Move best{};
    Score score{0};
    int16_t depth{-1};
    TTFlag flag{TTFlag::Exact};
};

class TT {
public:
    explicit TT(std::size_t mb = 16) { resize_mb(mb); }
    void resize_mb(std::size_t mb);
    TTEntry* probe(Key key);
    const TTEntry* probe(Key key) const;
    void store(Key key, int depth, Score score, TTFlag flag, Move best);

private:
    std::vector<TTEntry> table_;
    std::size_t mask_{0};
};

} // namespace chess
