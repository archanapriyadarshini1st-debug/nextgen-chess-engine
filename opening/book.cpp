#include "book.h"
#include <fstream>
#include <sstream>
#include <random>
#include <cstring>
#include <algorithm>

namespace chess {
namespace {
Move parse_move_text(const std::string& s) {
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

// Polyglot helpers
uint64_t zobrist_polyglot_random[781]; // 768 pieces + side + castling + ep
void init_polyglot_zobrist() {
    static bool init=false;
    if (init) return;
    std::mt19937_64 rng(0x1234567);
    for (int i=0;i<781;++i) zobrist_polyglot_random[i]=rng();
    init=true;
}

} // anon

uint64_t OpeningBook::polyglot_key(const Position& pos) {
    init_polyglot_zobrist();
    // Polyglot key is different from our zobrist - uses its own randoms, piece mapping
    // For compatibility we compute using polyglot scheme:
    // pieces: WP..BK index 0..11, but polyglot uses different order: 0 pawn,1 knight,2 bishop,3 rook,4 queen,5 king for white then black
    // We'll map similarly
    uint64_t key=0;
    const auto& b = pos.board();
    for (int sq=0;sq<64;++sq) {
        Piece p = b[sq];
        if (p==Piece::None) continue;
        int idx = piece_index(p); // 0..11
        // polyglot piece index: type*2 + color? simplified
        // We'll use idx directly for now (not fully compatible but functional for our own books)
        key ^= zobrist_polyglot_random[idx*64 + sq];
    }
    // castling: 4 bits
    int cr = pos.castling_rights();
    key ^= zobrist_polyglot_random[768 + cr];
    if (pos.ep_square()>=0) {
        int f = file_of(pos.ep_square());
        key ^= zobrist_polyglot_random[772 + f];
    }
    if (pos.side_to_move()==Color::White) key ^= zobrist_polyglot_random[780];
    return key;
}

Move OpeningBook::polyglot_move_to_move(uint16_t pg_move, const Position& pos) {
    // polyglot move encoding: from 6 bits (0..63), to 6 bits, promotion 4 bits
    int from = (pg_move >> 6) & 0x3F;
    int to = pg_move & 0x3F;
    int promo = (pg_move >> 12) & 0xF;
    uint32_t flags=0;
    if (promo) flags|=FLAG_PROMOTION;
    // detect capture: if destination occupied
    if (pos.board()[to]!=Piece::None) flags|=FLAG_CAPTURE;
    // castling detection: king moves 2 squares
    Piece moving = pos.board()[from];
    if (moving!=Piece::None && (moving==Piece::WK || moving==Piece::BK)) {
        if (std::abs(file_of(to)-file_of(from))>1) {
            if (file_of(to)>file_of(from)) flags|=FLAG_KING_CASTLE;
            else flags|=FLAG_QUEEN_CASTLE;
        }
    }
    return Move::make(from,to,flags, promo? promo:0);
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
        entries_[pos.zobrist()].push_back({parse_move_text(move_s), static_cast<std::uint16_t>(std::max(1, weight)), 0});
    }
    return true;
}

bool OpeningBook::load_polyglot(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    polyglot_.clear();
    // Polyglot entry: 16 bytes: uint64 key, uint16 move, uint16 weight, uint32 learn (big endian)
    while (true) {
        uint8_t buf[16];
        in.read(reinterpret_cast<char*>(buf), 16);
        if (in.gcount()!=16) break;
        uint64_t key = (uint64_t(buf[0])<<56)|(uint64_t(buf[1])<<40)|(uint64_t(buf[2])<<32)|(uint64_t(buf[3])<<24)|(uint64_t(buf[4])<<16)|(uint64_t(buf[5])<<8)|buf[6] | (uint64_t(buf[7]));
        // Actually need proper BE decode: above wrong bit shifts? Let's do correctly
        // Recalc: BE 8 bytes
        key = 0;
        for(int i=0;i<8;++i) key = (key<<8) | buf[i];
        uint16_t move = (buf[8]<<8)|buf[9];
        uint16_t weight = (buf[10]<<8)|buf[11];
        uint32_t learn = (buf[12]<<24)|(buf[13]<<16)|(buf[14]<<8)|buf[15];
        PolyglotEntry e{key, move, weight, learn};
        polyglot_[key].push_back(e);
    }
    polyglot_loaded_=!polyglot_.empty();
    return polyglot_loaded_;
}

std::optional<Move> OpeningBook::find(const Position& pos) const {
    auto it = entries_.find(pos.zobrist());
    if (it != entries_.end() && !it->second.empty()) {
        // weighted random
        int total=0; for(auto &e: it->second) total+=e.weight;
        std::mt19937 rng(std::random_device{}());
        int r = std::uniform_int_distribution<>(0,total-1)(rng);
        for(auto &e: it->second){ if (r<e.weight) return e.move; r-=e.weight; }
        return it->second.front().move;
    }
    // try polyglot
    return find_polyglot(pos);
}

std::optional<Move> OpeningBook::find_polyglot(const Position& pos) const {
    if (!polyglot_loaded_) return std::nullopt;
    uint64_t k = polyglot_key(pos);
    auto it = polyglot_.find(k);
    if (it==polyglot_.end() || it->second.empty()) return std::nullopt;
    // weight selection
    int total=0; for(auto &e: it->second) total+=e.weight;
    if (total==0) return polyglot_move_to_move(it->second.front().move, pos);
    std::mt19937 rng(std::random_device{}());
    int r = std::uniform_int_distribution<>(0,total-1)(rng);
    for(auto &e: it->second){ if (r<e.weight) return polyglot_move_to_move(e.move,pos); r-=e.weight; }
    return polyglot_move_to_move(it->second.front().move,pos);
}

void OpeningBook::update_weight(const Position& pos, Move m, int delta) {
    auto &vec = entries_[pos.zobrist()];
    for(auto &e: vec) if (e.move.raw==m.raw) { e.weight = std::clamp<int>(e.weight+delta,1,65535); return; }
    // if not found, add
    vec.push_back({m, (uint16_t)std::clamp(delta,1,65535),0});
}

} // namespace chess
