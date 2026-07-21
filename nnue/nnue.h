
#pragma once
#include "../core/position.h"
#include <string>

namespace chess {

class NNUE {
public:
    bool load(const std::string& path);
    Score evaluate(const Position& pos) const;
};

} // namespace chess
