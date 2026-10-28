#include "../core/movegen.h"
#include "../search/search.h"
#include "../opening/book.h"
#include "../tb/syzygy.h"
#include "../evaluation/eval.h"
#include "../nnue/nnue.h"
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace chess;

void test_startpos() {
    Position pos;
    pos.set_startpos();
    MoveList moves;
    generate_moves(pos, moves, false);
    assert(moves.size == 20);
    assert(perft(pos, 1) == 20);
    assert(perft(pos, 2) == 400);
    assert(perft(pos, 3) == 8902);
    assert(perft(pos, 4) == 197281);
}

void test_handcrafted_eval_side_to_move_symmetry() {
    // Swapping only the side-to-move on the symmetric start position must not
    // invent a positional advantage for White or Black.
    Position white_to_move, black_to_move;
    white_to_move.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    black_to_move.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    assert(evaluate_handcrafted(white_to_move) == evaluate_handcrafted(black_to_move));
}

void test_fen_roundtrip() {
    Position pos;
    pos.set_fen("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPP1PPP/RNBQKB1R b KQkq - 1 2");
    auto fen = pos.fen();
    Position copy;
    copy.set_fen(fen);
    assert(copy.fen() == fen);
}

void test_make_unmake_identity() {
    Position pos;
    pos.set_startpos();
    const auto fen0 = pos.fen();
    MoveList moves;
    generate_moves(pos, moves, false);
    assert(moves.size > 0);
    assert(pos.make_move(moves.moves[0]));
    pos.unmake_move();
    assert(pos.fen() == fen0);
}

void test_draw_detection() {
    Position pos;
    pos.set_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    assert(pos.is_insufficient_material());
    assert(pos.is_draw(0));

    pos.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 100 1");
    assert(pos.is_fifty_move());
    assert(pos.is_draw(0));
}

void test_threefold() {
    Position pos;
    pos.set_startpos();
    assert(!pos.is_threefold());
}

void test_null_move_does_not_enter_repetition_history() {
    Position pos;
    pos.set_startpos();
    const auto initial_fen = pos.fen();
    const auto initial_key = pos.zobrist();
    const auto initial_history_size = pos.key_history().size();

    assert(pos.make_null_move());
    assert(pos.zobrist() != initial_key);
    assert(pos.key_history().size() == initial_history_size);
    assert(!pos.is_threefold());

    pos.unmake_move();
    assert(pos.fen() == initial_fen);
    assert(pos.zobrist() == initial_key);
    assert(pos.key_history().size() == initial_history_size);
}
void test_capture_only_generation_keeps_quiet_promotions() {
    Position pos;
    pos.set_fen("7k/P7/8/8/8/8/8/7K w - - 0 1");

    MoveList tactical;
    generate_moves(pos, tactical, true);
    int quiet_promotions = 0;
    for (int i = 0; i < tactical.size; ++i) {
        const Move m = tactical.moves[i];
        if (m.from() == square_of(0, 6) && m.to() == square_of(0, 7) &&
            (m.flags() & FLAG_PROMOTION)) {
            ++quiet_promotions;
        }
    }
    assert(quiet_promotions == 4);
}
void test_nnue_binary_layout_and_score_scale() {
    const auto path = std::filesystem::temp_directory_path() / "nextgen_nnue_format_test.bin";
    std::vector<std::int16_t> ft_weights(NNUE_FT_SIZE * NNUE_HT1, 0);
    std::vector<std::int16_t> ft_bias(NNUE_HT1, 0);
    std::vector<std::int16_t> l1_weights(NNUE_HT1 * 2 * NNUE_HT2, 0);
    std::vector<std::int32_t> l1_bias(NNUE_HT2, 0);
    std::vector<std::int16_t> l2_weights(NNUE_HT2, 0);
    std::int32_t l2_bias = 0;

    // White-to-move: the white pawn on a7 is feature 48 in the us lane.
    ft_weights[48 * NNUE_HT1] = QA;
    l1_weights[0] = QB;
    l2_weights[0] = QB;
    // Black-to-move: the white pawn is feature 328 in the them lane;
    // use an independent negative path to verify STM perspective semantics.
    ft_weights[328 * NNUE_HT1 + 1] = QA;
    l1_weights[1 * NNUE_HT2 + 1] = QB;
    l2_weights[1] = -QB;

    {
        std::ofstream out(path, std::ios::binary);
        assert(out.good());
        const char magic[8] = {'N','G','N','N','U','E','0','2'};
        const std::uint32_t version = 2;
        out.write(magic, sizeof(magic));
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(ft_weights.data()),
                  static_cast<std::streamsize>(ft_weights.size() * sizeof(std::int16_t)));
        out.write(reinterpret_cast<const char*>(ft_bias.data()),
                  static_cast<std::streamsize>(ft_bias.size() * sizeof(std::int16_t)));
        out.write(reinterpret_cast<const char*>(l1_weights.data()),
                  static_cast<std::streamsize>(l1_weights.size() * sizeof(std::int16_t)));
        out.write(reinterpret_cast<const char*>(l1_bias.data()),
                  static_cast<std::streamsize>(l1_bias.size() * sizeof(std::int32_t)));
        out.write(reinterpret_cast<const char*>(l2_weights.data()),
                  static_cast<std::streamsize>(l2_weights.size() * sizeof(std::int16_t)));
        out.write(reinterpret_cast<const char*>(&l2_bias), sizeof(l2_bias));
        assert(out.good());
    }

    NNUE net;
    assert(net.load(path.string()));
    Position pos;
    pos.set_fen("7k/P7/8/8/8/8/8/7K w - - 0 1");
    assert(net.evaluate(pos) == 400);
    pos.set_fen("7k/P7/8/8/8/8/8/7K b - - 0 1");
    assert(net.evaluate(pos) == -400);

    // A trailing byte must invalidate a network rather than silently accepting
    // the prefix of an incompatible/oversized old file.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const char extra = 0;
        out.write(&extra, 1);
    }
    NNUE invalid;
    assert(!invalid.load(path.string()));
    std::filesystem::remove(path);
}


Move find_move_by_squares(Position& pos, int from, int to, int promo = 0) {
    MoveList legal;
    generate_moves(pos, legal, false);
    for (int i = 0; i < legal.size; ++i) {
        const Move m = legal.moves[i];
        if (m.from() == from && m.to() == to &&
            ((m.flags() & FLAG_PROMOTION) ? m.promo() == promo : promo == 0)) {
            return m;
        }
    }
    assert(false && "expected legal move not found");
    return Move{};
}

void test_see_exchange_sequences() {
    // e4xd5 is immediately a pawn win, but Black's e6 pawn recaptures.
    Position equal_pawn_trade;
    equal_pawn_trade.set_fen("4k3/8/4p3/3p4/4P3/8/8/4K3 w - - 0 1");
    Move pawn_capture = find_move_by_squares(
        equal_pawn_trade, square_of(4, 3), square_of(3, 4));
    assert((pawn_capture.flags() & FLAG_CAPTURE) != 0);
    assert(see(equal_pawn_trade, pawn_capture) == 0);
    assert(see_ge(equal_pawn_trade, pawn_capture, 0));

    // Qxd5 wins a pawn but loses the queen to c6xd5: net -800 cp.
    Position losing_queen_capture;
    losing_queen_capture.set_fen("4k3/8/2p5/3p4/8/8/8/3QK3 w - - 0 1");
    Move queen_capture = find_move_by_squares(
        losing_queen_capture, square_of(3, 0), square_of(3, 4));
    assert(see(losing_queen_capture, queen_capture) == -800);
    assert(!see_ge(losing_queen_capture, queen_capture, 0));

    // Quiet promotions are tactical too, even without a capture.
    Position quiet_promotion;
    quiet_promotion.set_fen("7k/P7/8/8/8/8/8/7K w - - 0 1");
    Move promote_queen = find_move_by_squares(
        quiet_promotion, square_of(0, 6), square_of(0, 7), 4);
    assert(see(quiet_promotion, promote_queen) == 800);
}

void test_search_finds_mate_in_one() {
    // White to move: Qg7# is mate. Quiescence search must not stand pat
    // while Black is in check, and must score a checked position with no
    // legal evasions as mate rather than as a static evaluation.
    Position pos;
    pos.set_fen("7k/8/5K2/6Q1/8/8/8/8 w - - 0 1");

    Search search;
    Limits limits;
    limits.depth = 1;
    const auto result = search.think(pos, limits);

    assert(!result.best_move.is_null());
    assert(result.score >= MATE_SCORE - 2);
    assert(pos.make_move(result.best_move));
    assert(pos.in_check(pos.side_to_move()));
    MoveList replies;
    generate_moves(pos, replies, false);
    assert(replies.size == 0);
}

void test_experience_book_preserves_castling_flags() {
    const std::string fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    Position pos;
    pos.set_fen(fen);

    const auto path = std::filesystem::temp_directory_path() / "nextgen_experience_book_test.txt";
    {
        std::ofstream out(path);
        assert(out.good());
        out << fen << " e1g1 500\n";
    }

    OpeningBook book;
    assert(book.load_text(path.string()));
    const auto move = book.find(pos);
    assert(move.has_value());
    assert((move->flags() & FLAG_KING_CASTLE) != 0);
    assert(pos.make_move(*move));
    assert(pos.piece_at(square_of(6, 0)) == Piece::WK);
    assert(pos.piece_at(square_of(5, 0)) == Piece::WR);
    assert(pos.piece_at(square_of(7, 0)) == Piece::None);
    pos.unmake_move();

    std::filesystem::remove(path);
}

void test_syzygy_stub_never_guesses_wins() {
    SyzygyTablebase tablebase;
    assert(!tablebase.set_path("."));

    Position queen_versus_king;
    queen_versus_king.set_fen("7k/8/8/8/8/8/6Q1/6K1 w - - 0 1");
    assert(!tablebase.probe_wdl(queen_versus_king).has_value());

    Position pawn_versus_king;
    pawn_versus_king.set_fen("7k/8/8/8/8/8/4P3/4K3 w - - 0 1");
    assert(!tablebase.probe_wdl(pawn_versus_king).has_value());
}

void test_kiwipete_perft() {
    Position pos;
    pos.set_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    assert(perft(pos,1)==48);
    assert(perft(pos,2)==2039);
    assert(perft(pos,3)==97862);
}

int main() {
    test_startpos();
    test_handcrafted_eval_side_to_move_symmetry();
    test_fen_roundtrip();
    test_make_unmake_identity();
    test_draw_detection();
    test_threefold();
    test_kiwipete_perft();
    test_search_finds_mate_in_one();
    test_see_exchange_sequences();
    test_nnue_binary_layout_and_score_scale();
    test_capture_only_generation_keeps_quiet_promotions();
    test_null_move_does_not_enter_repetition_history();
    test_experience_book_preserves_castling_flags();
    test_syzygy_stub_never_guesses_wins();
    std::cout << "engine regression tests passed (including perft, draw detection, repetition)\n";
    return 0;
}
