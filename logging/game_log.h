
#pragma once
#include "../core/types.h"
#include <fstream>
#include <string>
#include <vector>

namespace chess {

struct GameLogEntry {
    std::string game_id;
    int ply{0};
    std::string fen;
    std::string move;
    Score eval_cp{0};
    int depth{0};
    std::vector<std::string> pv;
    std::int64_t time_ms{0};
    std::string result;
    std::string phase;
    std::string tag;
};

class GameLogger {
public:
    explicit GameLogger(const std::string& path);
    void write(const GameLogEntry& entry);

private:
    std::ofstream out_;
};

} // namespace chess
