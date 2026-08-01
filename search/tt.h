#pragma once
#include "../core/position.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace chess {

enum class TTFlag : std::uint8_t { Exact = 0, Alpha = 1, Beta = 2 };

// Value snapshot handed back by probe(). We never hand out pointers into the
// table: with a genuinely shared TT another thread may overwrite a slot at any
// moment, so readers get a validated copy instead.
struct TTData {
    Move   best{};
    Score  score{0};
    Score  eval{0};
    int    depth{-1};
    TTFlag flag{TTFlag::Exact};
    bool   hit{false};
};

// 16-byte entry, 4 per 64-byte cache line.
// Lockless XOR trick: key holds (zobrist ^ data), so a torn read fails the
// (key ^ data) == zobrist check and is simply treated as a miss.
struct TTEntry {
    std::atomic<std::uint64_t> key{0};
    std::atomic<std::uint64_t> data{0};
};

struct alignas(64) TTCluster { TTEntry entries[4]; };

class TT {
public:
    explicit TT(std::size_t mb = 16) { resize_mb(mb); }

    void resize_mb(std::size_t mb);
    void clear();
    void new_search();

    TTData probe(Key key) const;
    void   store(Key key, int depth, Score score, TTFlag flag, Move best, Score eval);
    void   prefetch(Key key) const;
    std::size_t hashfull() const;
    std::size_t size_mb() const { return table_.size() * sizeof(TTCluster) / (1024 * 1024); }

    static std::uint64_t pack(Move best, Score score, Score eval, int depth, TTFlag flag, unsigned gen);
    static void unpack(std::uint64_t d, TTData& out, unsigned& gen);

private:
    std::vector<TTCluster> table_;
    std::size_t mask_{0};
    std::atomic<unsigned> generation_{0};
};

} // namespace chess
