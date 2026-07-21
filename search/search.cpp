#include "../core/position.h"
#include "../core/types.h"
#include <vector>

namespace chess {

struct SearchResult {
    Move best_move{};
    Score score{};
    std::vector<Move> pv;
    int depth = 0;
    std::int64_t nodes = 0;
};

class Search {
public:
    SearchResult think(Position&, const Limits&) { return {}; }
};

} // namespace chess
