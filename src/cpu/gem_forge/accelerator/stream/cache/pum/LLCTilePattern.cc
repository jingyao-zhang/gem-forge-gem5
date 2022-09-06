#include "LLCTilePattern.hh"

const size_t LLCTilePattern::NumBanks;

#define LLCTilePatternInit(dim)                                                \
  template <>                                                                  \
  LLCTilePatternImpl<dim, int64_t, LLCTilePattern::NumBanks>                   \
      &LLCTilePattern::init<dim>() {                                           \
    assert(this->dimension == 0);                                              \
    this->dimension = dim;                                                     \
    this->pat##dim = std::make_shared<Pat##dim>();                             \
    return *this->pat##dim;                                                    \
  }

LLCTilePatternInit(1);
LLCTilePatternInit(2);
LLCTilePatternInit(3);

#undef LLCTilePatternInit