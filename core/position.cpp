#include "position.h"

namespace chess {

void Position::set_startpos() {
    *this = Position{};
    stm_ = Color::White;
    fullmove_number_ = 1;
}

void Position::set_fen(const std::string&) {
    set_startpos();
}

std::string Position::fen() const {
    return "startpos";
}

bool Position::make_move(Move) {
    return true;
}

void Position::unmake_move() {}

bool Position::legal(Move) const {
    return true;
}

bool Position::in_check(Color) const {
    return false;
}

} // namespace chess
