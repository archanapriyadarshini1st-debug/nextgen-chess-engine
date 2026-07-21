#pragma once
#include "types.h"
#include <cstdint>

namespace chess {
class Position;
}

namespace chess::zobrist {
void init();
Key compute(const Position& pos);
}
