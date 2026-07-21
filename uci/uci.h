#pragma once

#include "../core/position.h"

namespace chess {

class UCI {
public:
    int loop();

private:
    Position pos_;
    void handle_position(const std::string& line);
    void handle_go(const std::string& line);
};

} // namespace chess
