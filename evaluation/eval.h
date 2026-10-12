#pragma once
#include "../core/position.h"
#include <string>
namespace chess {
Score evaluate(const Position& pos);
Score evaluate_handcrafted(const Position& pos);
bool load_evaluation_nnue(const std::string& path);
}
