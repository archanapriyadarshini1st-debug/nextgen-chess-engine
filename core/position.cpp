
#include "position.h"
#include "zobrist.h"
#include <cctype>
#include <sstream>

namespace chess {
namespace {
int idx(Piece p) { return piece_index(p); }
int sq(int f, int r) { return square_of(f, r); }
bool on(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }
char piece_char(Piece p) {
    switch (p) {
        case Piece::WP: return 'P'; case Piece::WN: return 'N'; case Piece::WB: return 'B'; case Piece::WR: return 'R'; case Piece::WQ: return 'Q'; case Piece::WK: return 'K';
        case Piece::BP: return 'p'; case Piece::BN: return 'n'; case Piece::BB: return 'b'; case Piece::BR: return 'r'; case Piece::BQ: return 'q'; case Piece::BK: return 'k';
        default: return '.';
    }
}
Piece char_piece(char c) {
    switch (c) {
        case 'P': return Piece::WP; case 'N': return Piece::WN; case 'B': return Piece::WB; case 'R': return Piece::WR; case 'Q': return Piece::WQ; case 'K': return Piece::WK;
        case 'p': return Piece::BP; case 'n': return Piece::BN; case 'b': return Piece::BB; case 'r': return Piece::BR; case 'q': return Piece::BQ; case 'k': return Piece::BK;
        default: return Piece::None;
    }
}
Piece promo_piece(Color c, std::uint32_t promo) {
    switch (promo) {
        case 1: return make_piece(c, 2);
        case 2: return make_piece(c, 3);
        case 3: return make_piece(c, 4);
        case 4: return make_piece(c, 5);
        default: return Piece::None;
    }
}
}

void Position::clear() {
    piece_bb_.fill(0);
    occ_.fill(0);
    board_.fill(Piece::None);
    stm_ = Color::White;
    castling_rights_ = 0;
    ep_square_ = -1;
    halfmove_clock_ = 0;
    fullmove_number_ = 1;
    key_ = 0;
    eval_cache_ = 0;
    history_.clear();
}

void Position::put_piece(int sq_, Piece p) {
    if (p == Piece::None) return;
    board_[sq_] = p;
    const int i = idx(p);
    piece_bb_[i] |= (1ULL << sq_);
    occ_[static_cast<int>(piece_color(p))] |= (1ULL << sq_);
}

void Position::remove_piece(int sq_) {
    Piece p = board_[sq_];
    if (p == Piece::None) return;
    const int i = idx(p);
    piece_bb_[i] &= ~(1ULL << sq_);
    occ_[static_cast<int>(piece_color(p))] &= ~(1ULL << sq_);
    board_[sq_] = Piece::None;
}

void Position::refresh_key() { key_ = zobrist::compute(*this); }

void Position::set_startpos() {
    set_fen("rn1qkbnr/pppbpppp/8/3p4/8/5NP1/PPPPPPBP/RNBQK2R w KQkq - 0 1");
    // The line above is a placeholder only if someone wants a concrete position.
    // Immediately reset to the actual chess starting position below.
    set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

void Position::set_fen(const std::string& fen) {
    clear();
    std::istringstream ss(fen);
    std::string board, side, castling, ep;
    if (!(ss >> board >> side >> castling >> ep)) { set_startpos(); return; }
    ss >> halfmove_clock_ >> fullmove_number_;

    int r = 7, f = 0;
    for (char c : board) {
        if (c == '/') { --r; f = 0; continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) { f += c - '0'; continue; }
        Piece p = char_piece(c);
        if (p != Piece::None && on(f, r)) put_piece(sq(f++, r), p);
    }

    stm_ = (side == "b") ? Color::Black : Color::White;
    castling_rights_ = 0;
    if (castling.find('K') != std::string::npos) castling_rights_ |= WHITE_KINGSIDE;
    if (castling.find('Q') != std::string::npos) castling_rights_ |= WHITE_QUEENSIDE;
    if (castling.find('k') != std::string::npos) castling_rights_ |= BLACK_KINGSIDE;
    if (castling.find('q') != std::string::npos) castling_rights_ |= BLACK_QUEENSIDE;
    if (ep != "-") {
        int file = ep[0] - 'a';
        int rank = ep[1] - '1';
        if (on(file, rank)) ep_square_ = sq(file, rank);
    }
    refresh_key();
}

std::string Position::fen() const {
    std::ostringstream out;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            Piece p = board_[sq(f, r)];
            if (p == Piece::None) {
                ++empty;
            } else {
                if (empty) { out << empty; empty = 0; }
                out << piece_char(p);
            }
        }
        if (empty) out << empty;
        if (r) out << '/';
    }
    out << ' ' << (stm_ == Color::White ? 'w' : 'b') << ' ';
    if (!castling_rights_) out << '-';
    else {
        if (castling_rights_ & WHITE_KINGSIDE) out << 'K';
        if (castling_rights_ & WHITE_QUEENSIDE) out << 'Q';
        if (castling_rights_ & BLACK_KINGSIDE) out << 'k';
        if (castling_rights_ & BLACK_QUEENSIDE) out << 'q';
    }
    out << ' ';
    if (ep_square_ < 0) out << '-';
    else out << char('a' + file_of(ep_square_)) << char('1' + rank_of(ep_square_));
    out << ' ' << halfmove_clock_ << ' ' << fullmove_number_;
    return out.str();
}

int Position::king_square(Color side) const {
    Piece k = side == Color::White ? Piece::WK : Piece::BK;
    for (int sq_ = 0; sq_ < 64; ++sq_) if (board_[sq_] == k) return sq_;
    return -1;
}

bool Position::square_attacked(int sq_, Color by) const {
    int f = file_of(sq_), r = rank_of(sq_);
    int pawn_dir = by == Color::White ? -1 : 1;
    int pr = r + pawn_dir;
    for (int df : {-1, 1}) {
        int nf = f + df;
        if (on(nf, pr)) {
            Piece p = board_[sq(nf, pr)];
            if (p == (by == Color::White ? Piece::WP : Piece::BP)) return true;
        }
    }

    static const int knight_d[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    for (auto& d : knight_d) {
        int nf = f + d[0], nr = r + d[1];
        if (on(nf, nr)) {
            Piece p = board_[sq(nf, nr)];
            if (p == (by == Color::White ? Piece::WN : Piece::BN)) return true;
        }
    }

    static const int king_d[8][2] = {{1,1},{1,0},{1,-1},{0,1},{0,-1},{-1,1},{-1,0},{-1,-1}};
    for (auto& d : king_d) {
        int nf = f + d[0], nr = r + d[1];
        if (on(nf, nr)) {
            Piece p = board_[sq(nf, nr)];
            if (p == (by == Color::White ? Piece::WK : Piece::BK)) return true;
        }
    }

    auto ray = [&](int df, int dr, Piece a, Piece b) {
        int nf = f + df, nr = r + dr;
        while (on(nf, nr)) {
            Piece p = board_[sq(nf, nr)];
            if (p != Piece::None) return p == a || p == b;
            nf += df; nr += dr;
        }
        return false;
    };

    if (ray(1,1, by == Color::White ? Piece::WB : Piece::BB, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(1,-1, by == Color::White ? Piece::WB : Piece::BB, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(-1,1, by == Color::White ? Piece::WB : Piece::BB, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(-1,-1, by == Color::White ? Piece::WB : Piece::BB, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(1,0, by == Color::White ? Piece::WR : Piece::BR, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(-1,0, by == Color::White ? Piece::WR : Piece::BR, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(0,1, by == Color::White ? Piece::WR : Piece::BR, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    if (ray(0,-1, by == Color::White ? Piece::WR : Piece::BR, by == Color::White ? Piece::WQ : Piece::BQ)) return true;
    return false;
}

bool Position::in_check(Color side) const {
    int ks = king_square(side);
    return ks >= 0 && square_attacked(ks, opposite(side));
}

bool Position::legal(Move m) const {
    Position copy = *this;
    return copy.make_move(m) && !copy.in_check(opposite(copy.side_to_move()));
}

bool Position::make_null_move() {
    Undo u;
    u.move = Move{};
    u.castling_rights = castling_rights_;
    u.ep_square = ep_square_;
    u.halfmove_clock = halfmove_clock_;
    u.fullmove_number = fullmove_number_;
    u.stm = stm_;
    u.key = key_;
    u.eval_cache = eval_cache_;
    history_.push_back(u);
    ep_square_ = -1;
    stm_ = opposite(stm_);
    if (u.stm == Color::Black) ++fullmove_number_;
    refresh_key();
    return true;
}

bool Position::make_move(Move m) {
    const int from = m.from();
    const int to = m.to();
    if (from < 0 || from >= 64 || to < 0 || to >= 64) return false;
    Piece moving = board_[from];
    if (moving == Piece::None) return false;
    if ((stm_ == Color::White && !is_white(moving)) || (stm_ == Color::Black && !is_black(moving))) return false;

    Undo u;
    u.move = m;
    u.castling_rights = castling_rights_;
    u.ep_square = ep_square_;
    u.halfmove_clock = halfmove_clock_;
    u.fullmove_number = fullmove_number_;
    u.stm = stm_;
    u.key = key_;
    u.eval_cache = eval_cache_;
    u.capture_square = -1;

    Piece captured = Piece::None;
    int capture_sq = to;
    if (m.flags() & FLAG_EN_PASSANT) capture_sq = stm_ == Color::White ? to - 8 : to + 8;
    if (capture_sq >= 0 && capture_sq < 64) captured = board_[capture_sq];
    if (captured != Piece::None) {
        u.captured = captured;
        u.capture_square = static_cast<int8_t>(capture_sq);
    }

    history_.push_back(u);

    halfmove_clock_ = (moving == Piece::WP || moving == Piece::BP || captured != Piece::None) ? 0 : static_cast<std::uint16_t>(halfmove_clock_ + 1);
    ep_square_ = -1;

    remove_piece(from);
    if (captured != Piece::None) remove_piece(capture_sq);

    if (m.flags() & FLAG_PROMOTION) {
        Piece promo = promo_piece(stm_, m.promo());
        if (promo == Piece::None) promo = make_piece(stm_, 4);
        put_piece(to, promo);
    } else {
        put_piece(to, moving);
    }

    if (m.flags() & FLAG_KING_CASTLE) {
        if (stm_ == Color::White) { remove_piece(7); put_piece(5, Piece::WR); }
        else { remove_piece(63); put_piece(61, Piece::BR); }
    } else if (m.flags() & FLAG_QUEEN_CASTLE) {
        if (stm_ == Color::White) { remove_piece(0); put_piece(3, Piece::WR); }
        else { remove_piece(56); put_piece(59, Piece::BR); }
    }

    if (moving == Piece::WK) castling_rights_ &= ~(WHITE_KINGSIDE | WHITE_QUEENSIDE);
    if (moving == Piece::BK) castling_rights_ &= ~(BLACK_KINGSIDE | BLACK_QUEENSIDE);
    if (moving == Piece::WR && from == 0) castling_rights_ &= ~WHITE_QUEENSIDE;
    if (moving == Piece::WR && from == 7) castling_rights_ &= ~WHITE_KINGSIDE;
    if (moving == Piece::BR && from == 56) castling_rights_ &= ~BLACK_QUEENSIDE;
    if (moving == Piece::BR && from == 63) castling_rights_ &= ~BLACK_KINGSIDE;
    if (captured == Piece::WR && capture_sq == 0) castling_rights_ &= ~WHITE_QUEENSIDE;
    if (captured == Piece::WR && capture_sq == 7) castling_rights_ &= ~WHITE_KINGSIDE;
    if (captured == Piece::BR && capture_sq == 56) castling_rights_ &= ~BLACK_QUEENSIDE;
    if (captured == Piece::BR && capture_sq == 63) castling_rights_ &= ~BLACK_KINGSIDE;

    if (moving == Piece::WP && to - from == 16) ep_square_ = from + 8;
    if (moving == Piece::BP && from - to == 16) ep_square_ = from - 8;

    stm_ = opposite(stm_);
    if (u.stm == Color::Black) ++fullmove_number_;
    refresh_key();
    return true;
}

void Position::unmake_move() {
    if (history_.empty()) return;
    Undo u = history_.back();
    history_.pop_back();
    if (u.move.is_null()) {
        castling_rights_ = u.castling_rights;
        ep_square_ = u.ep_square;
        halfmove_clock_ = u.halfmove_clock;
        fullmove_number_ = u.fullmove_number;
        stm_ = u.stm;
        key_ = u.key;
        eval_cache_ = u.eval_cache;
        return;
    }

    const int from = u.move.from();
    const int to = u.move.to();
    stm_ = u.stm;
    castling_rights_ = u.castling_rights;
    ep_square_ = u.ep_square;
    halfmove_clock_ = u.halfmove_clock;
    fullmove_number_ = u.fullmove_number;
    key_ = u.key;
    eval_cache_ = u.eval_cache;

    if (u.move.flags() & FLAG_KING_CASTLE) {
        if (stm_ == Color::White) { remove_piece(5); put_piece(7, Piece::WR); }
        else { remove_piece(61); put_piece(63, Piece::BR); }
    } else if (u.move.flags() & FLAG_QUEEN_CASTLE) {
        if (stm_ == Color::White) { remove_piece(3); put_piece(0, Piece::WR); }
        else { remove_piece(59); put_piece(56, Piece::BR); }
    }

    remove_piece(to);
    if (u.move.flags() & FLAG_PROMOTION) {
        put_piece(from, make_piece(stm_, 1));
    } else {
        put_piece(from, board_[to]);
    }
    if (u.captured != Piece::None && u.capture_square >= 0) put_piece(u.capture_square, u.captured);

    refresh_key();
}

} // namespace chess
