#pragma once
#include "../core/position.h"
#include <optional>
#include <string>
#include <vector>

namespace chess {

class SyzygyTablebase {
public:
    bool set_path(const std::string& path);
    std::optional<int> probe_wdl(const Position& pos) const; // -2 loss, -1 cursed, 0 draw, 1 win, 2 cursed win
    std::optional<int> probe_dtz(const Position& pos) const;
    bool is_loaded() const { return loaded_; }
    int max_pieces() const { return maxPieces_; }

    static bool init(const std::string& path);
    static unsigned probe_table(const Position& pos, unsigned &wdl, unsigned &dtz);

private:
    std::string path_;
    bool loaded_ = false;
    int maxPieces_ = 0;
};

} // namespace chess
