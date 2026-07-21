
#include "game_log.h"
#include <sstream>

namespace chess {
namespace {
std::string esc(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '\': o << "\\"; break;
            case '"': o << "\""; break;
            case '
': o << "\n"; break;
            case '': o << "\r"; break;
            case '	': o << "\t"; break;
            default: o << c; break;
        }
    }
    return o.str();
}
}

GameLogger::GameLogger(const std::string& path) : out_(path, std::ios::app) {}

void GameLogger::write(const GameLogEntry& e) {
    if (!out_) return;
    out_ << '{'
         << ""game_id":"" << esc(e.game_id) << ""," 
         << ""ply":" << e.ply << ','
         << ""fen":"" << esc(e.fen) << ""," 
         << ""move":"" << esc(e.move) << ""," 
         << ""eval_cp":" << e.eval_cp << ','
         << ""depth":" << e.depth << ','
         << ""time_ms":" << e.time_ms << ','
         << ""result":"" << esc(e.result) << ""," 
         << ""phase":"" << esc(e.phase) << ""," 
         << ""tag":"" << esc(e.tag) << ""," 
         << ""pv":[";
    for (std::size_t i = 0; i < e.pv.size(); ++i) {
        if (i) out_ << ',';
        out_ << '"' << esc(e.pv[i]) << '"';
    }
    out_ << "]}
";
}

} // namespace chess
