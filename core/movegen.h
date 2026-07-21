
#pragma once
#include "position.h"

namespace chess {
void generate_moves(Position& pos, MoveList& list, bool captures_only = false);
}
