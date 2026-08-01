#include "syzygy.h"
#include <atomic>
#include <iostream>
#include <string>

extern "C" {
#include "tbprobe.h"
}

namespace chess {
namespace {
std::string g_path;
std::atomic<bool> g_ready{false};
std::atomic<int>  g_largest{0};

struct Boards {
    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    unsigned count;
};

Boards to_fathom(const Position& pos) {
    Boards b{};
    b.white   = pos.occupancy(Color::White);
    b.black   = pos.occupancy(Color::Black);
    b.kings   = pos.pieces(Piece::WK) | pos.pieces(Piece::BK);
    b.queens  = pos.pieces(Piece::WQ) | pos.pieces(Piece::BQ);
    b.rooks   = pos.pieces(Piece::WR) | pos.pieces(Piece::BR);
    b.bishops = pos.pieces(Piece::WB) | pos.pieces(Piece::BB);
    b.knights = pos.pieces(Piece::WN) | pos.pieces(Piece::BN);
    b.pawns   = pos.pieces(Piece::WP) | pos.pieces(Piece::BP);
    b.count   = static_cast<unsigned>(__builtin_popcountll(b.white | b.black));
    return b;
}
} // anon

bool SyzygyTablebase::init(const std::string& p) {
    if (p.empty() || p == "<empty>") {
        g_ready.store(false);
        g_largest.store(0);
        return false;
    }
    g_path = p;
    const bool ok = tb_init(p.c_str());
    const int largest = static_cast<int>(TB_LARGEST);
    g_largest.store(largest);
    g_ready.store(ok && largest > 0);
    std::cout << "info string Syzygy path=" << p
              << " init=" << (ok ? "ok" : "failed")
              << " TB_LARGEST=" << largest << "\n" << std::flush;
    return g_ready.load();
}

void SyzygyTablebase::free_tb() {
    if (g_ready.exchange(false)) tb_free();
    g_largest.store(0);
}

bool SyzygyTablebase::is_ready() { return g_ready.load(std::memory_order_relaxed); }
int  SyzygyTablebase::max_pieces() { return g_largest.load(std::memory_order_relaxed); }
const std::string& SyzygyTablebase::path() { return g_path; }

std::optional<int> SyzygyTablebase::probe_wdl(const Position& pos) const {
    if (!is_ready()) return std::nullopt;

    // tb_probe_wdl() requires no castling rights and a zeroed 50-move counter.
    if (pos.castling_rights() != 0) return std::nullopt;
    if (pos.halfmove_clock() != 0) return std::nullopt;

    const Boards b = to_fathom(pos);
    if (b.count > static_cast<unsigned>(max_pieces())) return std::nullopt;

    const unsigned res = tb_probe_wdl(
        b.white, b.black, b.kings, b.queens, b.rooks, b.bishops, b.knights, b.pawns,
        /*rule50*/ 0u,
        /*castling*/ 0u,
        /*ep*/ pos.ep_square() > 0 ? static_cast<unsigned>(pos.ep_square()) : 0u,
        /*turn*/ pos.side_to_move() == Color::White);

    if (res == TB_RESULT_FAILED) return std::nullopt;
    return static_cast<int>(res) - 2;   // TB_LOSS..TB_WIN (0..4) -> -2..2
}

std::optional<int> SyzygyTablebase::probe_dtz(const Position& pos) const {
    if (!is_ready()) return std::nullopt;
    if (pos.castling_rights() != 0) return std::nullopt;

    const Boards b = to_fathom(pos);
    if (b.count > static_cast<unsigned>(max_pieces())) return std::nullopt;

    const unsigned res = tb_probe_root(
        b.white, b.black, b.kings, b.queens, b.rooks, b.bishops, b.knights, b.pawns,
        static_cast<unsigned>(pos.halfmove_clock()),
        0u,
        pos.ep_square() > 0 ? static_cast<unsigned>(pos.ep_square()) : 0u,
        pos.side_to_move() == Color::White,
        nullptr);

    if (res == TB_RESULT_FAILED) return std::nullopt;
    const int dtz = static_cast<int>(TB_GET_DTZ(res));
    const int wdl = static_cast<int>(TB_GET_WDL(res)) - 2;
    return wdl >= 0 ? dtz : -dtz;
}

} // namespace chess
