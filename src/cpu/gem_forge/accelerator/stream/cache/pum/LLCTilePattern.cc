#include "LLCTilePattern.hh"

namespace gem5 {

const size_t LLCTilePattern::NumBanks;

#define LLCTilePatternInit(dim)                                                \
  template <>                                                                  \
  LLCTilePatternImpl<dim, int64_t, LLCTilePattern::NumBanks, false>            \
      &LLCTilePattern::init<dim, false>() {                                    \
    assert(this->dimension == 0);                                              \
    this->dimension = dim;                                                     \
    this->aligned = false;                                                     \
    this->pat##dim = std::unique_ptr<Pat##dim>(new Pat##dim());                \
    return *this->pat##dim;                                                    \
  }

#define LLCTilePatternInitAligned(dim)                                         \
  template <>                                                                  \
  LLCTilePatternImpl<dim, int64_t, LLCTilePattern::NumBanks, true>             \
      &LLCTilePattern::init<dim, true>() {                                     \
    assert(this->dimension == 0);                                              \
    this->dimension = dim;                                                     \
    this->aligned = true;                                                      \
    this->pat##dim##A = std::unique_ptr<Pat##dim##A>(new Pat##dim##A());       \
    return *this->pat##dim##A;                                                 \
  }

LLCTilePatternInit(1);
LLCTilePatternInit(2);
LLCTilePatternInit(3);
LLCTilePatternInitAligned(1);
LLCTilePatternInitAligned(2);
LLCTilePatternInitAligned(3);

#undef LLCTilePatternInit
#undef LLCTilePatternInitAligned

} // namespace gem5

