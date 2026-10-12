#include "syzygy.h"
#include <iostream>

namespace chess {

// This translation unit is a compatibility stub, not a real Syzygy backend.
// Do not infer tablebase WDL from material alone: K+P vs K is not always a
// win, and returning mate-like scores for KQ/KR endings invents distances.
bool SyzygyTablebase::set_path(const std::string& path) {
    return init(path);
}

bool SyzygyTablebase::init(const std::string& path) {
    (void)path;
    loaded_ = false;
    maxPieces_ = 0;
    std::cout << "info string Syzygy probing unavailable: Fathom backend is not enabled\n";
    return false;
}

std::optional<int> SyzygyTablebase::probe_wdl(const Position& pos) const {
    // This rule is derivable without tablebase files. All other positions
    // must fall back to search until an actual Fathom probe is wired in.
    if (pos.is_insufficient_material()) return 0;
    return std::nullopt;
}

std::optional<int> SyzygyTablebase::probe_dtz(const Position& pos) const {
    (void)pos;
    return std::nullopt;
}

unsigned SyzygyTablebase::probe_table(const Position& pos, unsigned& wdl, unsigned& dtz) {
    SyzygyTablebase tb;
    auto result = tb.probe_wdl(pos);
    if (!result) return 0;
    wdl = static_cast<unsigned>(*result + 2); // legacy wrapper mapping
    dtz = 0; // no DTZ value exists in the stub
    return 1;
}

} // namespace chess
