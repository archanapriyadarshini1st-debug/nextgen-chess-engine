
#pragma once
#include "../core/position.h"
#include <optional>
#include <string>

namespace chess {

class SyzygyTablebase {
public:
    bool set_path(const std::string& path);
    std::optional<Score> probe_wdl(const Position& pos) const;
    std::optional<int> probe_dtz(const Position& pos) const;

private:
    std::string path_;
};

} // namespace chess
