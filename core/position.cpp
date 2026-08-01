#include "position.h"
#include "zobrist.h"
#include "bitboard.h"
#include <cctype>
#include <sstream>

namespace chess {
namespace {
inline int idx(Piece p) { return piece_index(p); }
inline int sq(int f, int r) { return square_of(f, r); }
inline bool on(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

void ensure_tables() {
    static const bool once = [] { zobrist::init(); ::Bitboard::init(); return true; }();
    (void)once;
}

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
// promo code 1..4 -> N B R Q
Piece promo_piece(Color c, std::uint32_t promo) {
    switch (promo) {
        case 1: return make_piece(c, 2);
        case 2: return make_piece(c, 3);
        case 3: return make_piece(c, 4);
        case 4: return make_piece(c, 5);
        default: return Piece::None;
    }
}
} // anon

Position::Position() { ensure_tables(); clear(); }

void Position::clear() {
    ensure_tables();
    piece_bb_.fill(0);
    occ_.fill(0);
    board_.fill(Piece::None);
    king_sq_ = {-1, -1};
    stm_ = Color::White;
    castling_rights_ = 0;
    ep_square_ = -1;
    halfmove_clock_ = 0;
    fullmove_number_ = 1;
    key_ = 0;
    eval_cache_ = 0;
    history_.clear();
    key_history_.clear();
    halfmove_history_.clear();
}

// ---- incremental zobrist lives here: every board mutation XORs its own key ----
void Position::put_piece(int s, Piece p) {
    if (p == Piece::None) return;
    const int i = idx(p);
    board_[s] = p;
    piece_bb_[i] |= (1ULL << s);
    occ_[static_cast<int>(piece_color(p))] |= (1ULL << s);
    key_ ^= zobrist::piece_key(i, s);
    if (p == Piece::WK) king_sq_[0] = s;
    else if (p == Piece::BK) king_sq_[1] = s;
}

void Position::remove_piece(int s) {
    Piece p = board_[s];
    if (p == Piece::None) return;
    const int i = idx(p);
    piece_bb_[i] &= ~(1ULL << s);
    occ_[static_cast<int>(piece_color(p))] &= ~(1ULL << s);
    board_[s] = Piece::None;
    key_ ^= zobrist::piece_key(i, s);
    if (p == Piece::WK) king_sq_[0] = -1;
    else if (p == Piece::BK) king_sq_[1] = -1;
}

Key Position::recompute_key() const { return zobrist::compute(*this); }

void Position::set_startpos() {
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
    if (ep != "-" && ep.size() >= 2) {
        int file = ep[0] - 'a';
        int rank = ep[1] - '1';
        if (on(file, rank)) ep_square_ = static_cast<std::int8_t>(sq(file, rank));
    }
    // pieces already folded into key_ by put_piece; add the state terms
    key_ ^= zobrist::castle_key(castling_rights_);
    if (ep_square_ >= 0) key_ ^= zobrist::ep_key(ep_square_);
    if (stm_ == Color::Black) key_ ^= zobrist::side_key();

    key_history_.push_back(key_);
    halfmove_history_.push_back(halfmove_clock_);
}

std::string Position::fen() const {
    std::ostringstream out;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            Piece p = board_[sq(f, r)];
            if (p == Piece::None) ++empty;
            else {
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

// ---- attacks: magic bitboards, no more 64-square mailbox ray walks ----
Bitboard Position::attackers_to(int s, Bitboard occ) const {
    return (::Bitboard::pawn_attacks[1][s] & pieces(Piece::WP))
         | (::Bitboard::pawn_attacks[0][s] & pieces(Piece::BP))
         | (::Bitboard::knight_attacks[s]  & (pieces(Piece::WN) | pieces(Piece::BN)))
         | (::Bitboard::king_attacks[s]    & (pieces(Piece::WK) | pieces(Piece::BK)))
         | (::Bitboard::bishop_attacks(s, occ) & (pieces(Piece::WB) | pieces(Piece::BB) | pieces(Piece::WQ) | pieces(Piece::BQ)))
         | (::Bitboard::rook_attacks(s, occ)   & (pieces(Piece::WR) | pieces(Piece::BR) | pieces(Piece::WQ) | pieces(Piece::BQ)));
}

bool Position::attacked_by(int s, Color by, Bitboard occ, Bitboard byMask) const {
    const int b = static_cast<int>(by);
    const Bitboard them = occ_[b] & byMask;
    if (::Bitboard::pawn_attacks[1 - b][s] & pieces(by, 1) & them) return true;
    if (::Bitboard::knight_attacks[s] & pieces(by, 2) & them) return true;
    if (::Bitboard::king_attacks[s]   & pieces(by, 6) & them) return true;
    const Bitboard bq = (pieces(by, 3) | pieces(by, 5)) & them;
    if (bq && (::Bitboard::bishop_attacks(s, occ) & bq)) return true;
    const Bitboard rq = (pieces(by, 4) | pieces(by, 5)) & them;
    if (rq && (::Bitboard::rook_attacks(s, occ) & rq)) return true;
    return false;
}

bool Position::square_attacked(int s, Color by) const {
    return attacked_by(s, by, occupancy_all(), ~0ULL);
}

bool Position::in_check(Color side) const {
    int ks = king_sq_[static_cast<int>(side)];
    return ks >= 0 && square_attacked(ks, opposite(side));
}

// Legality test for a pseudo-legal move without make/unmake.
bool Position::legal_pseudo(Move m) const {
    const int from = m.from(), to = m.to();
    const Piece pc = board_[from];
    if (pc == Piece::None) return false;
    const Color us = piece_color(pc);
    const Color them = opposite(us);

    // Castling legality (path/attacks) is fully validated during generation.
    if (m.flags() & (FLAG_KING_CASTLE | FLAG_QUEEN_CASTLE)) return true;

    const int capSq = (m.flags() & FLAG_EN_PASSANT)
                        ? (us == Color::White ? to - 8 : to + 8)
                        : to;

    Bitboard occ = occupancy_all();
    occ ^= (1ULL << from);
    occ &= ~(1ULL << capSq);
    occ |= (1ULL << to);

    const int ksq = (pc == Piece::WK || pc == Piece::BK) ? to : king_sq_[static_cast<int>(us)];
    if (ksq < 0) return false;

    const Bitboard byMask = ~(1ULL << capSq);
    return !attacked_by(ksq, them, occ, byMask);
}

bool Position::legal(Move m) const { return legal_pseudo(m); }

bool Position::is_threefold() const {
    int cnt = 0;
    for (auto k : key_history_) if (k == key_) { if (++cnt >= 3) return true; }
    return false;
}

bool Position::is_insufficient_material() const {
    const int total = __builtin_popcountll(occupancy_all());
    if (total == 2) return true;
    if (pieces(Piece::WP) | pieces(Piece::BP) | pieces(Piece::WR) | pieces(Piece::BR)
        | pieces(Piece::WQ) | pieces(Piece::BQ)) return false;
    if (total == 3) return true;                       // K+minor vs K
    if (total == 4) {
        if (count(Piece::WB) == 1 && count(Piece::BB) == 1) {
            const int wb = __builtin_ctzll(pieces(Piece::WB));
            const int bb = __builtin_ctzll(pieces(Piece::BB));
            if (((file_of(wb) + rank_of(wb)) & 1) == ((file_of(bb) + rank_of(bb)) & 1)) return true;
        }
        if (count(Piece::WN) == 2 || count(Piece::BN) == 2) return true;
    }
    return false;
}

bool Position::is_draw(int) const {
    if (halfmove_clock_ >= 100) return true;
    if (is_threefold()) return true;
    if (is_insufficient_material()) return true;
    return false;
}

bool Position::is_stalemate() const { return false; }
bool Position::is_checkmate() const { return false; }

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

    if (ep_square_ >= 0) key_ ^= zobrist::ep_key(ep_square_);
    ep_square_ = -1;
    key_ ^= zobrist::side_key();
    stm_ = opposite(stm_);
    if (u.stm == Color::Black) ++fullmove_number_;

    key_history_.push_back(key_);
    halfmove_history_.push_back(halfmove_clock_);
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
    if (m.flags() & FLAG_EN_PASSANT) capture_sq = (stm_ == Color::White) ? to - 8 : to + 8;
    if (capture_sq >= 0 && capture_sq < 64) captured = board_[capture_sq];
    if (captured != Piece::None) {
        u.captured = captured;
        u.capture_square = static_cast<int8_t>(capture_sq);
    }
    history_.push_back(u);

    // roll the old state terms out of the key first
    key_ ^= zobrist::castle_key(castling_rights_);
    if (ep_square_ >= 0) key_ ^= zobrist::ep_key(ep_square_);

    halfmove_clock_ = (moving == Piece::WP || moving == Piece::BP || captured != Piece::None)
                        ? 0 : static_cast<std::uint16_t>(halfmove_clock_ + 1);
    ep_square_ = -1;

    remove_piece(from);
    if (captured != Piece::None) remove_piece(capture_sq);

    if (m.flags() & FLAG_PROMOTION) {
        Piece promo = promo_piece(stm_, m.promo());
        if (promo == Piece::None) promo = make_piece(stm_, 5);   // default queen (was rook)
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

    if (moving == Piece::WP && to - from == 16) ep_square_ = static_cast<std::int8_t>(from + 8);
    if (moving == Piece::BP && from - to == 16) ep_square_ = static_cast<std::int8_t>(from - 8);

    // roll the new state terms in
    key_ ^= zobrist::castle_key(castling_rights_);
    if (ep_square_ >= 0) key_ ^= zobrist::ep_key(ep_square_);
    key_ ^= zobrist::side_key();

    stm_ = opposite(stm_);
    if (u.stm == Color::Black) ++fullmove_number_;

    key_history_.push_back(key_);
    halfmove_history_.push_back(halfmove_clock_);
    return true;
}

void Position::unmake_move() {
    if (history_.empty()) return;
    Undo u = history_.back();
    history_.pop_back();
    if (!key_history_.empty()) key_history_.pop_back();
    if (!halfmove_history_.empty()) halfmove_history_.pop_back();

    castling_rights_ = u.castling_rights;
    ep_square_ = u.ep_square;
    halfmove_clock_ = u.halfmove_clock;
    fullmove_number_ = u.fullmove_number;
    stm_ = u.stm;
    eval_cache_ = u.eval_cache;

    if (!u.move.is_null()) {
        const int from = u.move.from();
        const int to = u.move.to();
        Piece moved = board_[to];

        if (u.move.flags() & FLAG_KING_CASTLE) {
            if (stm_ == Color::White) { remove_piece(5); put_piece(7, Piece::WR); }
            else { remove_piece(61); put_piece(63, Piece::BR); }
        } else if (u.move.flags() & FLAG_QUEEN_CASTLE) {
            if (stm_ == Color::White) { remove_piece(3); put_piece(0, Piece::WR); }
            else { remove_piece(59); put_piece(56, Piece::BR); }
        }

        remove_piece(to);
        if (u.move.flags() & FLAG_PROMOTION) put_piece(from, make_piece(stm_, 1));
        else put_piece(from, moved);
        if (u.captured != Piece::None && u.capture_square >= 0) put_piece(u.capture_square, u.captured);
    }

    // put/remove above scribbled on key_; the saved key is authoritative.
    key_ = u.key;
}

} // namespace chess
