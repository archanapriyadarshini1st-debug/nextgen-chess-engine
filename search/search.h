#pragma once
#include "../core/position.h"
#include "tt.h"
#include <array>
#include <chrono>
#include <memory>
#include <vector>
namespace chess {
struct SearchResult { Move best_move{}; Score score{0}; std::vector<Move> pv; int depth{0}; int seldepth{0}; std::uint64_t nodes{0}; };
class Search {
public:
 Search():tt_(std::make_shared<TT>()){init_tables();}
 void set_hash_mb(std::size_t mb){tt_->resize_mb(mb);} void set_threads(unsigned n){threads_=n?n:1;} unsigned threads()const{return threads_;}
 void clear(){tt_->clear();for(auto&h:history_)for(auto&v:h)v=0;for(auto&k:killers_)for(auto&m:k)m=Move{};}
 SearchResult think(Position&,const Limits&); void set_chess960(bool b){chess960_=b;}
private:
 Score negamax(Position&,int,Score,Score,int,bool); Score qsearch(Position&,Score,Score,int); void order_moves(Position&,MoveList&,Move,int)const; int score_move(const Position&,Move,Move,int)const; std::vector<Move> extract_pv(Position,int)const; bool time_up()const; static int piece_slot(Piece); int complexity_score(const Position&)const; int adaptive_depth(const Position&,const Limits&)const; void init_tables();
 std::shared_ptr<TT> tt_; std::array<std::array<int,64>,12> history_{}; std::array<std::array<Move,2>,MAX_PLY> killers_{}; std::array<std::array<std::array<int,64>,12>,2> capture_history_{}; std::array<std::array<std::array<int,64>,64>,12> cont_history_{}; std::array<std::array<std::array<int,64>,64>,2> butterfly_{}; std::array<std::array<Move,64>,12> countermove_{}; Move prev_capture_move_{}; int prev_capture_square_{-1}; std::chrono::steady_clock::time_point start_{}; std::int64_t limit_ms_{0}; std::uint64_t nodes_{0}; unsigned threads_{1}; int seldepth_{0}; int lmr_table_[64][64]{}; int futility_margin_[16]{}; int reverse_futility_margin_[16]{}; int probcut_margin_[16]{}; bool chess960_{false};
};
} // namespace chess
