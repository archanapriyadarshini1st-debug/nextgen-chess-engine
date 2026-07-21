
#include "syzygy.h"

namespace chess {

bool SyzygyTablebase::set_path(const std::string& path) {
    path_ = path;
    return !path_.empty();
}

std::optional<Score> SyzygyTablebase::probe_wdl(const Position&) const { return std::nullopt; }
std::optional<int> SyzygyTablebase::probe_dtz(const Position&) const { return std::nullopt; }

} // namespace chess
