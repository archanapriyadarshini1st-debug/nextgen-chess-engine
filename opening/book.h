
#pragma once
#include "../core/position.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace chess {

struct BookEntry {
    Move move{};
    std::uint16_t weight{0};
};

class OpeningBook {
public:
    bool load_text(const std::string& path);
    std::optional<Move> find(const Position& pos) const;

private:
    std::unordered_map<Key, std::vector<BookEntry>> entries_;
};

} // namespace chess
