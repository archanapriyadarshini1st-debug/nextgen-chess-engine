#include "game_log.h"
#include <sstream>

namespace chess {
namespace {
std::string esc(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '\\': o << "\\\\"; break;
            case '"': o << "\\\""; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
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
         << "\"game_id\":\"" << esc(e.game_id) << "\","
         << "\"ply\":" << e.ply << ','
         << "\"fen\":\"" << esc(e.fen) << "\","
         << "\"move\":\"" << esc(e.move) << "\","
         << "\"eval_cp\":" << e.eval_cp << ','
         << "\"depth\":" << e.depth << ','
         << "\"seldepth\":" << e.seldepth << ','
         << "\"nodes\":" << e.nodes << ','
         << "\"hashfull\":" << e.hashfull << ','
         << "\"time_ms\":" << e.time_ms << ','
         << "\"nnue_eval\":" << e.nnue_eval << ','
         << "\"classical_eval\":" << e.classical_eval << ','
         << "\"result\":\"" << esc(e.result) << "\","
         << "\"phase\":\"" << esc(e.phase) << "\","
         << "\"tag\":\"" << esc(e.tag) << "\","
         << "\"tb_hit\":\"" << esc(e.tb_hit) << "\","
         << "\"book_move\":\"" << esc(e.book_move) << "\","
         << "\"time_management\":\"" << esc(e.time_management) << "\","
         << "\"pv\":[";
    for (std::size_t i = 0; i < e.pv.size(); ++i) {
        if (i) out_ << ',';
        out_ << '"' << esc(e.pv[i]) << '"';
    }
    out_ << "]}\n";
}

} // namespace chess
