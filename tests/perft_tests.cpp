#include "../core/movegen.h"
#include "../core/position.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace chess;

struct PerftCase {
    std::string fen;
    std::vector<uint64_t> nodes; // depth 1..n
    std::string name;
};

void run_suite() {
    std::vector<PerftCase> cases = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", {20,400,8902,197281,4865609}, "startpos"},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", {48,2039,97862,4085603}, "kiwipete"},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", {14,191,2812,43238}, "pos3"},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", {6,264,9467,422333}, "pos4"},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", {44,1486,62379}, "pos5"},
    };

    for (auto &c: cases) {
        Position pos;
        pos.set_fen(c.fen);
        std::cout << "Perft " << c.name << " " << c.fen << "\n";
        for (size_t d=0; d<c.nodes.size(); ++d) {
            uint64_t n = perft(pos, (int)d+1);
            std::cout << " depth " << d+1 << " got " << n << " expected " << c.nodes[d];
            if (n==c.nodes[d]) std::cout << " OK\n";
            else { std::cout << " FAIL\n"; assert(false); }
        }
    }
}

int main_perft() { run_suite(); return 0; }

// For compatibility with test_main, we keep main in test_main.cpp.
