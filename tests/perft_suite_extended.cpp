#include "../core/movegen.h"
#include "../core/position.h"
#include <iostream>
#include <vector>
#include <string>

using namespace chess;

struct PerftTest {
    std::string fen;
    std::vector<uint64_t> expected;
    std::string name;
};

int main() {
    std::vector<PerftTest> tests = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", {20,400,8902,197281,4865609}, "startpos"},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", {48,2039,97862,4085603}, "kiwipete"},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", {14,191,2812,43238,674624}, "pos3"},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", {6,264,9467,422333}, "pos4"},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", {44,1486,62379,2103487}, "pos5"},
        {"r4rk1/1pp1qppp/p1pb4/2p5/2Pp4/2P2N2/PP1Q1PPP/R1B1R1K1 w - - 0 1", {33,1174,38982}, "pos6 - pins corrected"},
        {"r4bk1/1p4p1/2p2r2/p7/P2PN3/1P1P1q2/3PKPp1/R1B5 w - - 2 25", {1}, "illegal e2f3 regression - should have 1 legal e2e1"},
        {"8/p1p5/P1b5/2p5/1PPb3K/5k2/8/4q3 w - - 0 60", {3}, "illegal f3f4 regression - should have 3 legal king moves"},
        {"rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR b KQkq e6 0 1", {27}, "en passant e6 corrected - python-chess says 27"},
        {"rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 2", {31}, "en passant f6 corrected - python-chess says 31"},
    };

    bool all_pass=true;
    for (auto &t: tests) {
        Position pos;
        pos.set_fen(t.fen);
        std::cout << "Testing " << t.name << " FEN: " << t.fen << "\n";
        for (size_t d=0; d<t.expected.size(); ++d) {
            uint64_t nodes = perft(pos, (int)d+1);
            std::cout << "  depth " << d+1 << " expected " << t.expected[d] << " got " << nodes;
            if (nodes==t.expected[d]) std::cout << " OK\n";
            else {
                std::cout << " FAIL\n";
                all_pass=false;
            }
        }
    }
    if (all_pass) std::cout << "All perft tests passed!\n";
    else std::cout << "Some perft tests FAILED!\n";
    return all_pass?0:1;
}
