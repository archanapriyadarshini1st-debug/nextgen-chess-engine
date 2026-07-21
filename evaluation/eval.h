#pragma once
#include "../core/position.h"
namespace chess {
Score evaluate(const Position& pos);
Score evaluate_handcrafted(const Position& pos);
}
