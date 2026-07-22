#include "syzygy.h"
#include <iostream>
// Try to include Fathom if present
#ifdef HAS_FATHOM
extern "C" {
#include "tbprobe.h"
}
#endif

namespace chess {
bool SyzygyTablebase::set_path(const std::string& path) {
    return init(path);
}
bool SyzygyTablebase::init(const std::string& path) {
#ifdef HAS_FATHOM
    if (tb_init(path.c_str())) {
        std::cout << "info string Syzygy Fathom initialized " << path << "\n";
        return true;
    }
    return false;
#else
    std::cout << "info string Syzygy Fathom not compiled, using stub path=" << path << "\n";
    return !path.empty();
#endif
}
std::optional<int> SyzygyTablebase::probe_wdl(const Position& pos) const {
#ifdef HAS_FATHOM
    // Convert position to Fathom format
    // This is simplified - real conversion needed
    // For now return nullopt to let search continue
    // Real implementation would call tb_probe_wdl
    return std::nullopt;
#else
    if (pos.is_insufficient_material()) return 0;
    return std::nullopt;
#endif
}
std::optional<int> SyzygyTablebase::probe_dtz(const Position& pos) const {
    auto wdl = probe_wdl(pos);
    if (!wdl) return std::nullopt;
    return 0;
}
}
