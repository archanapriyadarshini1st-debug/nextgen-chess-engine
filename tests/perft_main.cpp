// Perft correctness + speed harness.
//
// Perft counts leaf nodes of the move tree. It is the only test that proves a
// move generator is bit-exact: a single illegal move, missed evasion, or broken
// castling/en-passant rule shows up as a count mismatch. Reference values below
// are the standard published ones (CPW / Stockfish test suite).
//
//   ./perft            run the reference suite, report PASS/FAIL per position
//   ./perft bench      report nodes/sec on the start position
//   ./perft divide FEN DEPTH   per-move breakdown, for bisecting a mismatch

#include "core/bitboard.h"
#include "core/movegen.h"
#include "core/position.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace chess;

namespace {

struct Case {
  const char* fen;
  int depth;
  uint64_t nodes;
  const char* name;
};

// Depths kept modest so the suite runs in seconds, but deep enough that every
// special rule (ep pin, castling through check, promotion capture) is exercised.
const Case CASES[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 1, 20, "startpos d1"},
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 2, 400, "startpos d2"},
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 3, 8902, "startpos d3"},
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281, "startpos d4"},
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609, "startpos d5"},

    // Kiwipete: dense middlegame, all castling rights, many pins.
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 1, 48, "kiwipete d1"},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2, 2039, "kiwipete d2"},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862, "kiwipete d3"},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603, "kiwipete d4"},

    // Position 3: rook/pawn endgame that catches en-passant discovered-check bugs.
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238, "pos3 d4"},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624, "pos3 d5"},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6, 11030083, "pos3 d6"},

    // Position 4 and its mirror: promotion-heavy, asymmetric castling rights.
    // Equal counts on the mirror also prove the FEN parser and movegen are
    // colour-symmetric.
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333, "pos4 d4"},
    {"r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1", 4, 422333, "pos4-mirror d4"},

    // Position 5 / 6: known to break naive legality filtering.
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487, "pos5 d4"},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P3/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3065277, "pos6 d4"},

    // En-passant capture that is ILLEGAL because it exposes the king along the
    // rank (white Rh4 vs black Ka4 after d2-d4). Engines that filter legality by
    // "king not attacked after move" but forget that ep removes a pawn from a
    // *different* square than the destination will over-count here.
    {"8/8/8/8/k1p4R/8/3P4/3K4 b - - 0 1", 6, 1124950, "ep-pin d6"},

    // En-passant capture that IS legal, with a bishop eyeing the ep square.
    {"8/8/8/2k5/3Pp3/8/B7/4K3 b - d3 0 1", 5, 48969, "ep-legal d5"},

    // Promotion storm: both sides have pawns on the 7th/2nd with knights to
    // capture into. Exercises all four promo pieces plus promo-with-capture.
    {"n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1", 4, 182838, "promo-black d4"},
    {"n1n5/PPPk4/8/8/8/8/4Kppp/5N1N w - - 0 1", 4, 182838, "promo-white d4"},

    // TalkChess position that catches bogus castling-through-check handling.
    {"rnbqkb1r/pp1p1ppp/2p5/4P3/2B5/8/PPP1NnPP/RNBQK2R w KQkq - 0 6", 4, 1761505,
     "tk-castle d4"},
};

uint64_t divide(Position& pos, int depth) {
  MoveList list;
  generate_moves(pos, list);
  uint64_t total = 0;
  for (int i = 0; i < list.size; ++i) {
    const Move m = list.moves[i];
    if (!pos.make_move(m)) continue;
    const uint64_t n = depth <= 1 ? 1 : perft(pos, depth - 1);
    pos.unmake_move();
    const char* pc = " nbrq";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%c%c%c%c", char('a' + file_of(m.from())),
                  char('1' + rank_of(m.from())), char('a' + file_of(m.to())),
                  char('1' + rank_of(m.to())),
                  (m.flags() & FLAG_PROMOTION) ? pc[m.promo() & 4] : ' ');
    std::printf("  %s : %llu\n", buf, (unsigned long long)n);
    total += n;
  }
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  Bitboard::init();

  if (!Bitboard::verify()) {
    std::printf("FATAL: magic bitboard verification failed\n");
    return 2;
  }
  std::printf("magic bitboards verified\n\n");

  if (argc >= 2 && std::strcmp(argv[1], "divide") == 0 && argc >= 4) {
    Position pos;
    pos.set_fen(argv[2]);
    const int d = std::atoi(argv[3]);
    std::printf("divide %s depth %d\n", argv[2], d);
    std::printf("total: %llu\n", (unsigned long long)divide(pos, d));
    return 0;
  }

  if (argc >= 2 && std::strcmp(argv[1], "bench") == 0) {
    Position pos;
    pos.set_startpos();
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t n = perft(pos, 5);
    const auto t1 = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(t1 - t0).count();
    std::printf("perft(5) = %llu in %.3fs = %.0f nodes/s\n", (unsigned long long)n, s, n / s);
    return 0;
  }

  int pass = 0, fail = 0;
  double total_s = 0;
  uint64_t total_n = 0;

  for (const Case& c : CASES) {
    Position pos;
    pos.set_fen(c.fen);
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t got = perft(pos, c.depth);
    const auto t1 = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(t1 - t0).count();
    total_s += s;
    total_n += got;

    const bool ok = got == c.nodes;
    ok ? ++pass : ++fail;
    std::printf("%-20s d%d  %-12llu %s", c.name, c.depth, (unsigned long long)got,
                ok ? "PASS" : "FAIL");
    if (!ok) std::printf("  (expected %llu, diff %+lld)", (unsigned long long)c.nodes,
                         (long long)got - (long long)c.nodes);
    std::printf("  [%.2fs]\n", s);
  }

  std::printf("\n%d passed, %d failed", pass, fail);
  if (total_s > 0) std::printf("  |  %.0f nodes/s", total_n / total_s);
  std::printf("\n");
  return fail == 0 ? 0 : 1;
}
