#include "syzygy.h"
#include "tbprobe.h"
#include <iostream>

namespace chess {
namespace {
unsigned bb(const Position& p, Piece piece) { return 0; }
}

bool SyzygyTablebase::set_path(const std::string& path) { return init(path); }

bool SyzygyTablebase::init(const std::string& path) {
    if (path.empty()) return false;
    const bool ok = tb_init(path.c_str());
    if (ok) std::cout << "info string Syzygy Fathom initialized " << path << "\n";
    return ok;
}

std::optional<int> SyzygyTablebase::probe_wdl(const Position& pos) const {
    if (pos.is_insufficient_material()) return 0;
    if (pos.castling_rights() != 0) return std::nullopt;
    const auto white = pos.occupancy(Color::White);
    const auto black = pos.occupancy(Color::Black);
    const auto kings = pos.pieces(Piece::WK) | pos.pieces(Piece::BK);
    const auto queens = pos.pieces(Piece::WQ) | pos.pieces(Piece::BQ);
    const auto rooks = pos.pieces(Piece::WR) | pos.pieces(Piece::BR);
    const auto bishops = pos.pieces(Piece::WB) | pos.pieces(Piece::BB);
    const auto knights = pos.pieces(Piece::WN) | pos.pieces(Piece::BN);
    const auto pawns = pos.pieces(Piece::WP) | pos.pieces(Piece::BP);
    const unsigned ep = pos.ep_square() >= 0 ? static_cast<unsigned>(pos.ep_square()) : 0;
    const unsigned result = tb_probe_wdl(white, black, kings, queens, rooks, bishops, knights, pawns,
                                         pos.halfmove_clock(), 0, ep,
                                         pos.side_to_move() == Color::White);
    if (result == TB_RESULT_FAILED) return std::nullopt;
    const int wdl = static_cast<int>(TB_GET_WDL(result));
    return wdl > 0 ? 1 : (wdl < 0 ? -1 : 0);
}

std::optional<int> SyzygyTablebase::probe_dtz(const Position& pos) const {
    return probe_wdl(pos).has_value() ? std::optional<int>(0) : std::nullopt;
}
}
