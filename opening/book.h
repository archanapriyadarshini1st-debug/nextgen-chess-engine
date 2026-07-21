#pragma once
#include "../core/position.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

namespace chess {

struct BookEntry {
    Move move{};
    std::uint16_t weight{0};
    std::uint32_t learn{0};
};

struct PolyglotEntry {
    uint64_t key;
    uint16_t move;
    uint16_t weight;
    uint32_t learn;
};

class OpeningBook {
public:
    bool load_text(const std::string& path);
    bool load_polyglot(const std::string& path); // loads .bin
    std::optional<Move> find(const Position& pos) const;
    std::optional<Move> find_polyglot(const Position& pos) const;
    bool is_polyglot_loaded() const { return polyglot_loaded_; }

    // learning
    void update_weight(const Position& pos, Move m, int delta);

private:
    std::unordered_map<Key, std::vector<BookEntry>> entries_;
    std::unordered_map<uint64_t, std::vector<PolyglotEntry>> polyglot_;
    bool polyglot_loaded_=false;

    static uint64_t polyglot_key(const Position& pos);
    static Move polyglot_move_to_move(uint16_t pg_move, const Position& pos);
    static uint16_t move_to_polyglot(Move m);
};

} // namespace chess
