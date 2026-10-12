#include "../core/movegen.h"
#include "../search/search.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace chess;

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
    pos.set_fen("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPP1PPP/RNBQKB1R b KQkq - 1 2");
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

void test_draw_detection() {
    Position pos;
    pos.set_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    assert(pos.is_insufficient_material());
    assert(pos.is_draw(0));

    pos.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 100 1");
    assert(pos.is_fifty_move());
    assert(pos.is_draw(0));
}

void test_threefold() {
    Position pos;
    pos.set_startpos();
    assert(!pos.is_threefold());
}

void test_search_finds_mate_in_one() {
    // White to move: Qg7# is mate. Quiescence search must not stand pat
    // while Black is in check, and must score a checked position with no
    // legal evasions as mate rather than as a static evaluation.
    Position pos;
    pos.set_fen("7k/8/5K2/6Q1/8/8/8/8 w - - 0 1");

    Search search;
    Limits limits;
    limits.depth = 1;
    const auto result = search.think(pos, limits);

    assert(result.best_move.raw == Move::make(square_of(6, 4), square_of(6, 6)).raw);
    assert(result.score >= MATE_SCORE - 2);
}

void test_kiwipete_perft() {
    Position pos;
    pos.set_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    assert(perft(pos,1)==48);
    assert(perft(pos,2)==2039);
    assert(perft(pos,3)==97862);
}

int main() {
    test_startpos();
    test_fen_roundtrip();
    test_make_unmake_identity();
    test_draw_detection();
    test_threefold();
    test_kiwipete_perft();
    test_search_finds_mate_in_one();
    std::cout << "engine regression tests passed (including perft, draw detection, repetition)\n";
    return 0;
}
