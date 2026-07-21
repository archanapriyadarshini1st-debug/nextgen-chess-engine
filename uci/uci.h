
#pragma once
#include "../core/position.h"
#include "../search/search.h"
#include <string>

namespace chess {

class UCI {
public:
    int loop();

private:
    Position pos_;
    Search search_;

    void handle_position(const std::string& line);
    void handle_go(const std::string& line);
    void handle_setoption(const std::string& line);
    bool apply_uci_move(const std::string& token);
    static int parse_square(const std::string& s);
    static std::string square_string(int sq);
    static char promo_char(int promo);
};

} // namespace chess
