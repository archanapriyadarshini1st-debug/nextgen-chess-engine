#pragma once
#include "../core/position.h"
#include <string>
namespace chess {
Score evaluate(const Position& pos);
Score evaluate_handcrafted(const Position& pos);
Score evaluate_white_relative(const Position& pos);   // exposed for the symmetry test
bool  load_nnue_file(const std::string& path);
void  set_external_eval(const std::string& path);     // opt-in, off by default
}
