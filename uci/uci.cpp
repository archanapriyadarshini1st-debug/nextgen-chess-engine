#include "uci.h"
#include "../core/movegen.h"
#include "../tb/syzygy.h"
#include "../opening/book.h"
#include "../nnue/nnue.h"
#include <iostream>
#include <sstream>
#include <chrono>

namespace chess {
namespace {
std::string join_pv(const std::vector<Move>& pv) {
    std::ostringstream out;
    for (std::size_t i = 0; i < pv.size(); ++i) {
        if (i) out << ' ';
        out << UCI::square_string(pv[i].from()) << UCI::square_string(pv[i].to());
        if (pv[i].flags() & FLAG_PROMOTION) out << UCI::promo_char(pv[i].promo());
    }
    return out.str();
}

uint64_t bench_search(Search& search, const std::vector<std::string>& fens, int depth) {
    uint64_t totalNodes=0;
    auto start = std::chrono::steady_clock::now();
    for (auto &fen: fens) {
        Position pos;
        pos.set_fen(fen);
        Limits lim; lim.depth=depth;
        SearchResult r = search.think(pos, lim);
        totalNodes += r.nodes;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    std::cout << "info string bench depth " << depth << " total nodes " << totalNodes << " time " << elapsed << "ms nps " << (elapsed? totalNodes*1000/elapsed:0) << "\n";
    return totalNodes;
}
}

int UCI::parse_square(const std::string& s) {
    if (s.size() < 2) return -1;
    int f = s[0] - 'a';
    int r = s[1] - '1';
    if (f < 0 || f >= 8 || r < 0 || r >= 8) return -1;
    return square_of(f, r);
}

std::string UCI::square_string(int sq) {
    std::string s = "  ";
    s[0] = char('a' + file_of(sq));
    s[1] = char('1' + rank_of(sq));
    return s;
}

char UCI::promo_char(int promo) {
    switch (promo) { case 1: return 'n'; case 2: return 'b'; case 3: return 'r'; case 4: return 'q'; default: return '\0'; }
}

bool UCI::apply_uci_move(const std::string& token) {
    MoveList moves;
    generate_moves(pos_, moves, false);
    const std::string from = token.substr(0, 2);
    const std::string to = token.substr(2, 2);
    int from_sq = parse_square(from);
    int to_sq = parse_square(to);
    char promo = token.size() >= 5 ? token[4] : '\0';
    for (int i = 0; i < moves.size; ++i) {
        Move m = moves.moves[i];
        if (m.from() != from_sq || m.to() != to_sq) continue;
        if (m.flags() & FLAG_PROMOTION) {
            if (promo_char(m.promo()) != promo) continue;
        } else if (promo != '\0') continue;
        return pos_.make_move(m);
    }
    return false;
}

void UCI::handle_position(const std::string& line) {
    std::istringstream ss(line);
    std::string cmd, token;
    ss >> cmd >> token;
    if (token == "startpos") {
        pos_.set_startpos();
    } else if (token == "fen") {
        std::string fen, part;
        for (int i = 0; i < 6 && ss >> part; ++i) {
            if (i) fen += ' ';
            fen += part;
        }
        pos_.set_fen(fen);
    }
    while (ss >> token) if (token == "moves") break;
    while (ss >> token) apply_uci_move(token);
}

void UCI::handle_setoption(const std::string& line) {
    std::istringstream ss(line);
    std::string tok, name, value;
    ss >> tok;
    ss >> tok; // name
    std::string word;
    bool readingValue=false;
    name="";
    value="";
    while (ss >> word) {
        if (word=="value") { readingValue=true; continue; }
        if (!readingValue) {
            if (!name.empty()) name += ' ';
            name += word;
        } else {
            if (!value.empty()) value += ' ';
            value += word;
        }
    }
    if (name == "Hash") {
        try { search_.set_hash_mb(static_cast<std::size_t>(std::stoul(value))); } catch (...) {}
    } else if (name == "Threads") {
        try { search_.set_threads(static_cast<unsigned>(std::stoul(value))); } catch (...) {}
    } else if (name == "SyzygyPath") {
        SyzygyTablebase::init(value);
    } else if (name == "BookFile") {
        book_.load_polyglot(value);
    } else if (name == "NNUEFile") {
        nnue_.load(value);
    } else if (name == "OwnBook") {
        use_book_ = (value=="true" || value=="True");
    } else if (name == "UCI_Chess960") {
        chess960_ = (value=="true" || value=="True");
        search_.set_chess960(chess960_);
    } else if (name == "MultiPV") {
        try { multiPV_ = std::stoi(value); } catch (...) {}
    } else if (name == "Skill Level") {
        try { skillLevel_ = std::stoi(value); } catch (...) {}
    }
}

void UCI::handle_go(const std::string& line) {
    Limits limits;
    std::istringstream ss(line);
    std::string token;
    ss >> token;
    while (ss >> token) {
        if (token == "depth") ss >> limits.depth;
        else if (token == "movetime") ss >> limits.movetime_ms;
        else if (token == "wtime") ss >> limits.wtime_ms;
        else if (token == "btime") ss >> limits.btime_ms;
        else if (token == "winc") ss >> limits.winc_ms;
        else if (token == "binc") ss >> limits.binc_ms;
        else if (token == "movestogo") ss >> limits.movestogo;
        else if (token == "infinite") limits.infinite = true;
    }

    // Book probe first
    if (use_book_) {
        if (auto bm = book_.find(pos_)) {
            std::cout << "bestmove " << square_string(bm->from()) << square_string(bm->to());
            if (bm->flags() & FLAG_PROMOTION) std::cout << promo_char(bm->promo());
            std::cout << "\n";
            return;
        }
    }

    if (multiPV_ > 1) {
        // MultiPV: find N best moves
        MoveList root;
        generate_moves(pos_, root, false);
        std::vector<std::pair<Move, Score>> scored;
        for (int i=0;i<root.size;++i) {
            Position p = pos_;
            if (!p.make_move(root.moves[i])) continue;
            Limits l; l.depth = limits.depth>0? limits.depth: 6;
            SearchResult r = search_.think(p, l);
            scored.emplace_back(root.moves[i], -r.score);
        }
        std::sort(scored.begin(), scored.end(), [](auto &a, auto &b){ return a.second > b.second; });
        for (int i=0;i<std::min((int)scored.size(), multiPV_); ++i) {
            std::cout << "info multipv " << i+1 << " depth " << limits.depth << " score cp " << scored[i].second << " pv " << square_string(scored[i].first.from()) << square_string(scored[i].first.to()) << "\n";
        }
        if (!scored.empty()) {
            std::cout << "bestmove " << square_string(scored[0].first.from()) << square_string(scored[0].first.to());
            if (scored[0].first.flags() & FLAG_PROMOTION) std::cout << promo_char(scored[0].first.promo());
            std::cout << "\n";
            return;
        }
    }

    // Skill level: if <20, occasionally pick not best move
    SearchResult r = search_.think(pos_, limits);
    if (skillLevel_ < 20 && !r.pv.empty() && r.pv.size()>1) {
        // 20 is max, 0 is weakest - add randomness
        int idx = (20 - skillLevel_) / 5;
        if (idx>0 && r.pv.size()> (size_t)idx) {
            // pick move at idx instead of best with some probability
            if (rand()%100 < (20-skillLevel_)*5) {
                r.best_move = r.pv[std::min((size_t)idx, r.pv.size()-1)];
            }
        }
    }

    std::cout << "info depth " << r.depth << " seldepth " << r.seldepth << " score cp " << r.score << " nodes " << r.nodes;
    if (!r.pv.empty()) std::cout << " pv " << join_pv(r.pv);
    std::cout << "\n";
    if (r.best_move.is_null()) std::cout << "bestmove 0000\n";
    else {
        std::cout << "bestmove " << square_string(r.best_move.from()) << square_string(r.best_move.to());
        if (r.best_move.flags() & FLAG_PROMOTION) std::cout << promo_char(r.best_move.promo());
        std::cout << "\n";
    }
}

int UCI::loop() {
    pos_.set_startpos();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "uci") {
            std::cout << "id name NextGenChessEngine\n";
            std::cout << "id author CodePassionat\n";
            std::cout << "option name Hash type spin default 16 min 1 max 1024\n";
            std::cout << "option name Threads type spin default 1 min 1 max 128\n";
            std::cout << "option name SyzygyPath type string default <empty>\n";
            std::cout << "option name SyzygyProbeDepth type spin default 1 min 1 max 100\n";
            std::cout << "option name BookFile type string default <empty>\n";
            std::cout << "option name NNUEFile type string default <empty>\n";
            std::cout << "option name OwnBook type check default false\n";
            std::cout << "option name MultiPV type spin default 1 min 1 max 5\n";
            std::cout << "option name Skill Level type spin default 20 min 0 max 20\n";
            std::cout << "option name UCI_Elo type spin default 3190 min 1320 max 3190\n";
            std::cout << "option name UCI_LimitStrength type check default false\n";
            std::cout << "option name Move Overhead type spin default 10 min 0 max 5000\n";
            std::cout << "option name Ponder type check default false\n";
            std::cout << "option name UCI_Chess960 type check default false\n";
            std::cout << "uciok\n";
        } else if (line == "isready") {
            std::cout << "readyok\n";
        } else if (line.rfind("setoption", 0) == 0) {
            handle_setoption(line);
        } else if (line.rfind("position", 0) == 0) {
            handle_position(line);
        } else if (line.rfind("go", 0) == 0) {
            handle_go(line);
        } else if (line == "ucinewgame") {
            pos_.set_startpos();
            search_.clear();
        } else if (line=="bench") {
            std::vector<std::string> fens = {
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
                "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1"
            };
            bench_search(search_, fens, 10);
            std::cout << "bestmove 0000\n";
        } else if (line.rfind("perft",0)==0) {
            std::istringstream ss(line);
            std::string tok; int depth=1;
            ss >> tok >> depth;
            uint64_t nodes = 0;
            // simple perft
            Position p = pos_;
            extern uint64_t perft(Position& pos, int depth);
            nodes = perft(p, depth);
            std::cout << "perft " << depth << " nodes " << nodes << "\n";
        } else if (line == "quit") {
            break;
        } else if (line=="d") {
            std::cout << pos_.fen() << "\n";
        }
    }
    return 0;
}

} // namespace chess
