
#include "book.h"
#include <fstream>
#include <sstream>

namespace chess {
namespace {
Move parse_move(const std::string& s) {
    if (s.size() < 4) return Move{};
    int from = square_of(s[0] - 'a', s[1] - '1');
    int to = square_of(s[2] - 'a', s[3] - '1');
    std::uint32_t promo = 0;
    if (s.size() >= 5) {
        switch (s[4]) {
            case 'n': promo = 1; break;
            case 'b': promo = 2; break;
            case 'r': promo = 3; break;
            case 'q': promo = 4; break;
            default: break;
        }
    }
    return Move::make(from, to, promo ? FLAG_PROMOTION : 0, promo);
}
}

bool OpeningBook::load_text(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    entries_.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fen, move_s; int weight = 1;
        if (!(ss >> fen >> move_s)) continue;
        ss >> weight;
        Position pos; pos.set_fen(fen);
        entries_[pos.zobrist()].push_back({parse_move(move_s), static_cast<std::uint16_t>(std::max(1, weight))});
    }
    return true;
}

std::optional<Move> OpeningBook::find(const Position& pos) const {
    auto it = entries_.find(pos.zobrist());
    if (it == entries_.end() || it->second.empty()) return std::nullopt;
    Move best = it->second.front().move;
    std::uint16_t best_w = it->second.front().weight;
    for (const auto& e : it->second) if (e.weight > best_w) { best_w = e.weight; best = e.move; }
    return best;
}

} // namespace chess
