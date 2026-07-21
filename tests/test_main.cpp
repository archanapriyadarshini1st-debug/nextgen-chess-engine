
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

void test_startpos() {
    Position pos;
    pos.set_startpos();
    MoveList moves;
    generate_moves(pos, moves, false);
    assert(moves.size == 20);
    assert(perft(pos, 1) == 20);
    assert(perft(pos, 2) == 400);
    assert(perft(pos, 3) == 8902);
    assert(perft(pos, 4) == 197281);
}

void test_fen_roundtrip() {
    Position pos;
    pos.set_fen("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
    auto fen = pos.fen();
    Position copy;
    copy.set_fen(fen);
    assert(copy.fen() == fen);
}

void test_make_unmake_identity() {
    Position pos;
    pos.set_startpos();
    const auto fen0 = pos.fen();
    MoveList moves;
    generate_moves(pos, moves, false);
    assert(moves.size > 0);
    assert(pos.make_move(moves.moves[0]));
    pos.unmake_move();
    assert(pos.fen() == fen0);
}

int main() {
    test_startpos();
    test_fen_roundtrip();
    test_make_unmake_identity();
    std::cout << "engine regression tests passed
";
    return 0;
}
