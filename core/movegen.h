#pragma once
#include "position.h"

namespace chess {
void generate_moves(Position& pos, MoveList& list, bool captures_only = false);
int see(const Position& pos, Move m);
bool see_ge(const Position& pos, Move m, int threshold);
uint64_t perft(Position& pos, int depth);
}
