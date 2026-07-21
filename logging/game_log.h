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
    int seldepth{0};
    std::vector<std::string> pv;
    std::int64_t time_ms{0};
    std::string result;
    std::string phase;
    std::string tag;
    // new fields for Stockfish-19 level logging
    Score nnue_eval{0};
    Score classical_eval{0};
    int nodes{0};
    int hashfull{0};
    std::string tb_hit;
    std::string book_move;
    std::string time_management;
};

class GameLogger {
public:
    explicit GameLogger(const std::string& path);
    void write(const GameLogEntry& entry);
    void flush() { if (out_) out_.flush(); }

private:
    std::ofstream out_;
};

} // namespace chess
