#include "syzygy.h"
#include <iostream>
#ifdef HAS_FATHOM
extern "C" {
#include "tbprobe.h"
}
#endif

namespace chess {
bool SyzygyTablebase::set_path(const std::string& path) { return init(path); }
bool SyzygyTablebase::init(const std::string& path) {
#ifdef HAS_FATHOM
    return !path.empty() && tb_init(path.c_str());
#else
    std::cout << "info string Syzygy Fathom unavailable in this build, path=" << path << "\n";
    return false;
#endif
}
std::optional<int> SyzygyTablebase::probe_wdl(const Position& pos) const {
#ifdef HAS_FATHOM
    if (pos.castling_rights() != 0) return std::nullopt;
    const auto white=pos.occupancy(Color::White), black=pos.occupancy(Color::Black);
    const auto kings=pos.pieces(Piece::WK)|pos.pieces(Piece::BK);
    const auto queens=pos.pieces(Piece::WQ)|pos.pieces(Piece::BQ);
    const auto rooks=pos.pieces(Piece::WR)|pos.pieces(Piece::BR);
    const auto bishops=pos.pieces(Piece::WB)|pos.pieces(Piece::BB);
    const auto knights=pos.pieces(Piece::WN)|pos.pieces(Piece::BN);
    const auto pawns=pos.pieces(Piece::WP)|pos.pieces(Piece::BP);
    const unsigned ep=pos.ep_square()>=0?static_cast<unsigned>(pos.ep_square()):0;
    const unsigned r=tb_probe_wdl(white,black,kings,queens,rooks,bishops,knights,pawns,pos.halfmove_clock(),0,ep,pos.side_to_move()==Color::White);
    if (r==TB_RESULT_FAILED) return std::nullopt;
    const int w=static_cast<int>(TB_GET_WDL(r)); return w>0?1:(w<0?-1:0);
#else
    if (pos.is_insufficient_material()) return 0;
    return std::nullopt;
#endif
}
std::optional<int> SyzygyTablebase::probe_dtz(const Position& pos) const { return probe_wdl(pos).has_value()?std::optional<int>(0):std::nullopt; }
}
