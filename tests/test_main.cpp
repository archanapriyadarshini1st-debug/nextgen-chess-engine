// Regression suite: perft, incremental zobrist, eval mirror symmetry, SEE,
// move ordering cost, and shared-TT multithreaded search.
#include "../core/movegen.h"
#include "../core/bitboard.h"
#include "../evaluation/eval.h"
#include "../search/search.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace chess;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// ---------------------------------------------------------------- perft ----
struct PerftCase { const char* fen; int depth; unsigned long long nodes; };
static const PerftCase PERFT[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6, 11030083},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5, 15833292},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
};

static void test_perft() {
    printf("perft\n");
    unsigned long long total = 0;
    double ms = 0;
    for (const auto& c : PERFT) {
        Position p; p.set_fen(c.fen);
        auto t0 = std::chrono::steady_clock::now();
        const unsigned long long got = perft(p, c.depth);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total += got;
        char buf[256];
        snprintf(buf, sizeof buf, "depth %d -> %llu (want %llu)", c.depth, got, c.nodes);
        check(got == c.nodes, buf);
        check(p.key_ok(), "zobrist key intact after perft");
    }
    printf("  %llu nodes, %.0f ms, %.2f Mnps\n", total, ms, total / ms / 1000.0);
}

// ------------------------------------------------------ incremental key ----
static void walk_keys(Position& pos, int depth, int& bad) {
    if (depth == 0) return;
    MoveList l;
    generate_moves(pos, l, false);
    for (int i = 0; i < l.size; ++i) {
        if (!pos.make_move(l.moves[i])) continue;
        if (!pos.key_ok()) ++bad;
        walk_keys(pos, depth - 1, bad);
        pos.unmake_move();
        if (!pos.key_ok()) ++bad;
    }
}

static void test_incremental_zobrist() {
    printf("incremental zobrist\n");
    int bad = 0;
    for (const auto& c : PERFT) {
        Position p; p.set_fen(c.fen);
        walk_keys(p, 3, bad);
    }
    check(bad == 0, "incremental key matches full rebuild at every node");
}

// ------------------------------------------------------- eval symmetry ----
static std::string mirror_fen(const std::string& fen) {
    std::istringstream ss(fen);
    std::string board, stm, castle, ep, hm = "0", fm = "1";
    ss >> board >> stm >> castle >> ep; ss >> hm >> fm;

    std::vector<std::string> ranks;
    std::string cur;
    for (char c : board) { if (c == '/') { ranks.push_back(cur); cur.clear(); } else cur += c; }
    ranks.push_back(cur);
    std::reverse(ranks.begin(), ranks.end());
    std::string nb;
    for (std::size_t i = 0; i < ranks.size(); ++i) {
        if (i) nb += '/';
        for (char c : ranks[i]) nb += std::isalpha((unsigned char)c)
            ? (std::isupper((unsigned char)c) ? (char)std::tolower(c) : (char)std::toupper(c)) : c;
    }
    std::string nc;
    for (char c : castle) nc += std::isalpha((unsigned char)c)
        ? (std::isupper((unsigned char)c) ? (char)std::tolower(c) : (char)std::toupper(c)) : c;
    std::string ne = ep;
    if (ep != "-" && ep.size() >= 2) ne = std::string(1, ep[0]) + char('1' + (7 - (ep[1] - '1')));
    return nb + " " + (stm == "w" ? "b" : "w") + " " + nc + " " + ne + " " + hm + " " + fm;
}

static const char* SYM_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
    "2rq1rk1/pp1bppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/2KR1B1R w - - 0 1",
    "8/5p2/4p1p1/3pP1Pp/2pP1P1P/2P5/8/8 w - - 0 1",
    "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 0 1",
};

static void test_eval_symmetry() {
    printf("eval mirror symmetry\n");
    int worst = 0;
    std::string worstFen;
    for (const char* f : SYM_FENS) {
        Position a, b;
        a.set_fen(f);
        b.set_fen(mirror_fen(f));
        const int va = evaluate_white_relative(a);
        const int vb = evaluate_white_relative(b);
        const int diff = std::abs(va + vb);      // must be exactly antisymmetric
        if (diff > worst) { worst = diff; worstFen = f; }
        const int ha = evaluate_handcrafted(a);
        const int hb = evaluate_handcrafted(b);
        if (std::abs(ha - hb) > 0) { worst = std::max(worst, std::abs(ha - hb)); worstFen = f; }
    }
    char buf[300];
    snprintf(buf, sizeof buf, "max mirror asymmetry = %d cp (was up to 74)%s%s",
             worst, worst ? " worst=" : "", worst ? worstFen.c_str() : "");
    check(worst == 0, buf);
}

// ------------------------------------------------------------------ SEE ----
static void test_see() {
    printf("static exchange evaluation\n");
    struct S { const char* fen; const char* mv; bool ge0; };
    const S cases[] = {
        {"1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1", "e1e5", true},   // Rxe5 wins a pawn
        {"1k1r3q/1ppn3p/p4b2/4p3/8/P2N2P1/1PP1R1BP/2K1Q3 w - - 0 1", "d3e5", false}, // Nxe5 loses material
        {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", true},                 // free pawn
    };
    for (const auto& c : cases) {
        Position p; p.set_fen(c.fen);
        MoveList l; generate_moves(p, l, false);
        bool found = false, got = false;
        for (int i = 0; i < l.size; ++i) {
            char s[8];
            snprintf(s, sizeof s, "%c%c%c%c",
                     'a' + file_of(l.moves[i].from()), '1' + rank_of(l.moves[i].from()),
                     'a' + file_of(l.moves[i].to()),   '1' + rank_of(l.moves[i].to()));
            if (std::string(s) == c.mv) { found = true; got = see_ge(p, l.moves[i], 0); break; }
        }
        check(found && got == c.ge0, std::string("see_ge(") + c.mv + ") == " + (c.ge0 ? "true" : "false"));
    }
}

// --------------------------------------------------------------- search ----
static void test_search() {
    printf("search\n");
    Position p; p.set_fen("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
    Search s; s.set_hash_mb(16);
    Limits lim; lim.depth = 8;
    auto t0 = std::chrono::steady_clock::now();
    auto r = s.think(p, lim);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  depth %d  %llu nodes  %.0f ms  %.0f nps  score %d\n",
           r.depth, (unsigned long long)r.nodes, ms, r.nodes / (ms / 1000.0), r.score);
    check(!r.best_move.is_null(), "returns a move");
    check(r.depth >= 8, "reaches the requested depth");

    // mate in 1
    Position m; m.set_fen("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
    Search s2; s2.set_hash_mb(8);
    Limits l2; l2.depth = 5;
    auto r2 = s2.think(m, l2);
    check(r2.score > 900, "finds the winning rook endgame");
}

static void test_lazy_smp() {
    printf("lazy smp (shared TT)\n");
    Position p; p.set_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    Search s; s.set_hash_mb(32); s.set_threads(4);
    Limits lim; lim.depth = 7;
    auto r = s.think(p, lim);
    printf("  4 threads: depth %d  %llu nodes  score %d  hashfull %zu\n",
           r.depth, (unsigned long long)r.nodes, r.score, s.tt().hashfull());
    check(!r.best_move.is_null(), "multithreaded search returns a move");
    check(p.key_ok(), "root position untouched");
}

int main() {
    ::Bitboard::init();
    test_perft();
    test_incremental_zobrist();
    test_eval_symmetry();
    test_see();
    test_search();
    test_lazy_smp();
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
