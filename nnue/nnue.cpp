
#include "nnue.h"

namespace chess {

bool NNUE::load(const std::string&) { return true; }
Score NNUE::evaluate(const Position&) const { return 0; }

} // namespace chess
