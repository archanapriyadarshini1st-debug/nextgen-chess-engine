#pragma once
#include "../core/position.h"
#include <string>
#include <memory>

namespace chess {

class StockfishEvaluator {
public:
    static StockfishEvaluator& instance();
    bool init(const std::string& stockfish_path = "/tmp/stockfish/stockfish-ubuntu-x86-64-avx2");
    Score evaluate(const Position& pos);
    bool is_available() const { return available_; }
    ~StockfishEvaluator();

private:
    StockfishEvaluator() = default;
    bool available_=false;
    int to_child_fd_=-1;
    int from_child_fd_=-1;
    pid_t pid_=-1;
    void send(const std::string& cmd);
    std::string recv_until(const std::string& marker, int timeout_ms=1000);
};

} // namespace chess
