#pragma once
#include "../core/position.h"
#include <optional>
#include <string>

namespace chess {

// Thin wrapper over Fathom (tb/tbprobe.c). Previously probe_wdl() was a stub
// that returned nullopt; it now performs a real tb_probe_wdl().
class SyzygyTablebase {
public:
    // -2 loss, -1 blessed loss, 0 draw, 1 cursed win, 2 win (side-to-move POV)
    std::optional<int> probe_wdl(const Position& pos) const;
    std::optional<int> probe_dtz(const Position& pos) const;

    static bool init(const std::string& path);   // calls tb_init()
    static void free_tb();
    static bool is_ready();                      // loaded AND has usable tables
    static int  max_pieces();
    static const std::string& path();

    bool set_path(const std::string& p) { return init(p); }
    bool is_loaded() const { return is_ready(); }
};

} // namespace chess
