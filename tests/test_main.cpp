
#include "../core/movegen.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using namespace chess;

std::uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;
    MoveList moves;
    generate_moves(pos, moves, false);
    if (depth == 1) return static_cast<std::uint64_t>(moves.size);
    std::uint64_t nodes = 0;
    for (int i = 0; i < moves.size; ++i) {
        if (!pos.make_move(moves.moves[i])) continue;
        nodes += perft(pos, depth - 1);
        pos.unmake_move();
    }
    return nodes;
}

int main() {
    Position pos;
    pos.set_startpos();
    MoveList moves;
    generate_moves(pos, moves, false);
    assert(moves.size == 20);
    assert(perft(pos, 2) == 400);
    assert(perft(pos, 3) == 8902);
    std::cout << "basic engine tests passed
";
    return 0;
}
