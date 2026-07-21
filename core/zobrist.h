
#pragma once
#include "types.h"

namespace chess::zobrist {
void init();
Key compute(const class Position& pos);
}
