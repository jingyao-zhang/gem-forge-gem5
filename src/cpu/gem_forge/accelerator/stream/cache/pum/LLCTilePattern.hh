#ifndef __CPU_GEM_FORGE_LLC_TILE_PATTERN_HH__
#define __CPU_GEM_FORGE_LLC_TILE_PATTERN_HH__

#include "AffinePattern.hh"

#include <array>
#include <memory>

#include "base/logging.hh"

namespace gem5 {

/**
 * Implements a template of mapped tile pattern among LLC banks.
 *
 * There are some assumptions:
 * 1. Each LLC bank is in charge of some continuous tiles.
 * 2. Based on 1, for dimension D, there will be at most 3^(D-1) subregions
 * belongs to that LLC bank.
 *
 * Hence, we can know what is the maximal number of splited source patterns for
 * a fixed dimension.
 */

template <size_t D, typename T, size_t banks, bool aligned>
class LLCTilePatternImpl {
public:
  using AffPatImpl = AffinePatternImpl<D, T>;

  // So far we only support 3D array.
  static_assert(D > 0);
  static_assert(D < 4);

  static constexpr size_t MaxSubRegions =
      aligned ? 1 : AffinePatternImpl<D, T>::MaxSubRegionsForContinuousRange;
  // static constexpr size_t MaxSubRegions = 1;

  struct BankPattern {
    std::array<AffPatImpl, MaxSubRegions> patterns;
  };

  std::array<BankPattern, banks> bankPatterns;
  std::array<int, banks> bankPatCounts;

  LLCTilePatternImpl() {
    for (auto &b : bankPatCounts) {
      b = 0;
    }
  }

  size_t getBankSubRegionCount(int bankIdx) const {
    return bankPatCounts.at(bankIdx);
  }

  const AffPatImpl &getAffinePatternImpl(int bankIdx, int patternIdx) const {
    return bankPatterns.at(bankIdx).patterns.at(patternIdx);
  }

  AffinePattern getAffinePattern(int bankIdx, int patternIdx) const {
    const auto &pat = this->getAffinePatternImpl(bankIdx, patternIdx);
    return getAffinePatternFromImpl(pat);
  }

  void clearBankSubRegion(int bankIdx) { bankPatCounts.at(bankIdx) = 0; }

  void addPattern(int bankIdx, const AffPatImpl &pat) {
    auto &b = bankPatterns.at(bankIdx);
    auto &count = bankPatCounts.at(bankIdx);
    assert(count < MaxSubRegions);
    b.patterns[count] = pat;
    count++;
  }
};

class LLCTilePatternGeneric {
public:
  std::vector<std::vector<AffinePattern>> bankPatterns;
  LLCTilePatternGeneric(int banks) { bankPatterns.resize(banks); }

  size_t getBankSubRegionCount(int bankIdx) const {
    return bankPatterns.at(bankIdx).size();
  }

  AffinePattern getAffinePattern(int bankIdx, int patternIdx) const {
    return bankPatterns.at(bankIdx).at(patternIdx);
  }

  void clearBankSubRegion(int bankIdx) { bankPatterns.at(bankIdx).clear(); }

  void addPattern(int bankIdx, const AffinePattern &pat) {
    auto &b = bankPatterns.at(bankIdx);
    b.push_back(pat);
  }
};

class LLCTilePattern {

public:
  static constexpr size_t NumBanks = 64;
  using Pat1 = LLCTilePatternImpl<1, int64_t, NumBanks, false>;
  using Pat2 = LLCTilePatternImpl<2, int64_t, NumBanks, false>;
  using Pat3 = LLCTilePatternImpl<3, int64_t, NumBanks, false>;
  using Pat1A = LLCTilePatternImpl<1, int64_t, NumBanks, true>;
  using Pat2A = LLCTilePatternImpl<2, int64_t, NumBanks, true>;
  using Pat3A = LLCTilePatternImpl<3, int64_t, NumBanks, true>;
  using PatG = LLCTilePatternGeneric;

  size_t dimension = 0;
  bool aligned = false;
  std::unique_ptr<Pat1> pat1 = nullptr;
  std::unique_ptr<Pat2> pat2 = nullptr;
  std::unique_ptr<Pat3> pat3 = nullptr;
  std::unique_ptr<Pat1A> pat1A = nullptr;
  std::unique_ptr<Pat2A> pat2A = nullptr;
  std::unique_ptr<Pat3A> pat3A = nullptr;
  std::unique_ptr<PatG> patG = nullptr;

  LLCTilePattern() : dimension(0), aligned(false) {}

#define COPY_PAT(Pat, pat)                                                     \
  if (other.pat) {                                                             \
    pat = std::unique_ptr<Pat>(new Pat(*other.pat));                           \
  }

  LLCTilePattern(const LLCTilePattern &other)
      : dimension(other.dimension), aligned(other.aligned) {
    COPY_PAT(Pat1, pat1);
    COPY_PAT(Pat2, pat2);
    COPY_PAT(Pat3, pat3);
    COPY_PAT(Pat1A, pat1A);
    COPY_PAT(Pat2A, pat2A);
    COPY_PAT(Pat3A, pat3A);
    COPY_PAT(PatG, patG);
  }

  LLCTilePattern &operator=(const LLCTilePattern &other) {
    this->dimension = other.dimension;
    this->aligned = other.aligned;
    COPY_PAT(Pat1, pat1);
    COPY_PAT(Pat2, pat2);
    COPY_PAT(Pat3, pat3);
    COPY_PAT(Pat1A, pat1A);
    COPY_PAT(Pat2A, pat2A);
    COPY_PAT(Pat3A, pat3A);
    COPY_PAT(PatG, patG);
    return *this;
  }

#undef COPY_PAT

  void initialize(size_t dimension, bool aligned = false) {

    assert(this->dimension == 0);
    this->dimension = dimension;
    this->aligned = aligned;

#define LLCTilePatternInitCase(dim)                                            \
  case dim: {                                                                  \
    if (aligned) {                                                             \
      pat##dim##A = std::unique_ptr<Pat##dim##A>(new Pat##dim##A());           \
    } else {                                                                   \
      pat##dim = std::unique_ptr<Pat##dim>(new Pat##dim());                    \
    }                                                                          \
    break;                                                                     \
  }

    switch (dimension) {
      LLCTilePatternInitCase(1);
      LLCTilePatternInitCase(2);
      LLCTilePatternInitCase(3);
    default: {
      patG = std::unique_ptr<PatG>(new PatG(NumBanks));
      break;
    }
    }

#undef LLCTilePatternInit
  }

  template <size_t dimension, bool aligned>
  LLCTilePatternImpl<dimension, int64_t, NumBanks, aligned> &init() {
    // This should never be initialized.
    panic("Should never instantiate.");
  }

#define LLCTilePatternDispRet(func, args...)                                   \
  assert(this->dimension != 0);                                                \
  if (this->aligned) {                                                         \
    switch (this->dimension) {                                                 \
    case 1: {                                                                  \
      return this->pat1A->func(args);                                          \
    }                                                                          \
    case 2: {                                                                  \
      return this->pat2A->func(args);                                          \
    }                                                                          \
    case 3: {                                                                  \
      return this->pat3A->func(args);                                          \
    }                                                                          \
    default: {                                                                 \
      return this->patG->func(args);                                           \
    }                                                                          \
    }                                                                          \
  } else {                                                                     \
    switch (this->dimension) {                                                 \
    case 1: {                                                                  \
      return this->pat1->func(args);                                           \
    }                                                                          \
    case 2: {                                                                  \
      return this->pat2->func(args);                                           \
    }                                                                          \
    case 3: {                                                                  \
      return this->pat3->func(args);                                           \
    }                                                                          \
    default: {                                                                 \
      return this->patG->func(args);                                           \
    }                                                                          \
    }                                                                          \
  }

#define LLCTilePatternDispVoid(func, args...)                                  \
  assert(this->dimension != 0);                                                \
  if (this->aligned) {                                                         \
    switch (this->dimension) {                                                 \
    case 1: {                                                                  \
      this->pat1A->func(args);                                                 \
      break;                                                                   \
    }                                                                          \
    case 2: {                                                                  \
      this->pat2A->func(args);                                                 \
      break;                                                                   \
    }                                                                          \
    case 3: {                                                                  \
      this->pat3A->func(args);                                                 \
      break;                                                                   \
    }                                                                          \
    default: {                                                                 \
      this->patG->func(args);                                                  \
      break;                                                                   \
    }                                                                          \
    }                                                                          \
  } else {                                                                     \
    switch (this->dimension) {                                                 \
    case 1: {                                                                  \
      this->pat1->func(args);                                                  \
      break;                                                                   \
    }                                                                          \
    case 2: {                                                                  \
      this->pat2->func(args);                                                  \
      break;                                                                   \
    }                                                                          \
    case 3: {                                                                  \
      this->pat3->func(args);                                                  \
      break;                                                                   \
    }                                                                          \
    default: {                                                                 \
      this->patG->func(args);                                                  \
      break;                                                                   \
    }                                                                          \
    }                                                                          \
  }

  size_t getBankSubRegionCount(int bankIdx) const {
    if (this->dimension == 0) {
      // Not initialized yet.
      return 0;
    }
    LLCTilePatternDispRet(getBankSubRegionCount, bankIdx);
  }

  AffinePattern getAffinePattern(int bankIdx, int patternIdx) const {
    LLCTilePatternDispRet(getAffinePattern, bankIdx, patternIdx);
  }

  void clearBankSubRegion(int bankIdx) {
    LLCTilePatternDispVoid(clearBankSubRegion, bankIdx);
  }

  void addPattern(int bankIdx, const AffinePattern &pat) {
    // Only exposed to generic pattern.
    assert(this->dimension > 3);
    this->patG->addPattern(bankIdx, pat);
  }

#undef LLCTilePatternDispRet
#undef LLCTilePatternDispVoid
};

#define LLCTilePatternInit(dim)                                                \
  template <>                                                                  \
  LLCTilePatternImpl<dim, int64_t, LLCTilePattern::NumBanks, false>            \
      &LLCTilePattern::init<dim, false>();

#define LLCTilePatternInitAligned(dim)                                         \
  template <>                                                                  \
  LLCTilePatternImpl<dim, int64_t, LLCTilePattern::NumBanks, true>             \
      &LLCTilePattern::init<dim, true>();

LLCTilePatternInit(1);
LLCTilePatternInit(2);
LLCTilePatternInit(3);
LLCTilePatternInitAligned(1);
LLCTilePatternInitAligned(2);
LLCTilePatternInitAligned(3);

#undef LLCTilePatternInit
#undef LLCTilePatternInitAligned

} // namespace gem5

#endif