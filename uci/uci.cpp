#include "uci.h"
#include <iostream>
#include <sstream>

namespace chess {

int UCI::loop() {
    pos_.set_startpos();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "uci") {
            std::cout << "id name NextGenChessEngine\n";
            std::cout << "id author OpenAI\n";
            std::cout << "uciok\n";
        } else if (line == "isready") {
            std::cout << "readyok\n";
        } else if (line.rfind("position", 0) == 0) {
            handle_position(line);
        } else if (line.rfind("go", 0) == 0) {
            handle_go(line);
        } else if (line == "ucinewgame") {
            pos_.set_startpos();
        } else if (line == "quit") {
            break;
        }
    }
    return 0;
}

void UCI::handle_position(const std::string& line) {
    if (line.find("startpos") != std::string::npos) {
        pos_.set_startpos();
    }
}

void UCI::handle_go(const std::string&) {
    std::cout << "bestmove 0000\n";
}

} // namespace chess
