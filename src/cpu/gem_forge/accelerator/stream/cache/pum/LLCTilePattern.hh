#ifndef __CPU_GEM_FORGE_LLC_TILE_PATTERN_HH__
#define __CPU_GEM_FORGE_LLC_TILE_PATTERN_HH__

#include "AffinePattern.hh"

#include <array>
#include <memory>

#include "base/logging.hh"

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

template <size_t D, typename T, size_t banks> class LLCTilePatternImpl {
public:
  using AffPatImpl = AffinePatternImpl<D, T>;

  // So far we only support 3D array.
  static_assert(D > 0);
  static_assert(D < 4);

  static constexpr size_t MaxSubRegions =
      AffinePatternImpl<D, T>::MaxSubRegionsForContinuousRange;

  struct BankPattern {
    std::array<AffPatImpl, MaxSubRegions> patterns;
    int count = 0;
    BankPattern() : count(0) {}
  };

  std::array<BankPattern, banks> bankPatterns;

  LLCTilePatternImpl() {
    for (auto &b : bankPatterns) {
      b.count = 0;
    }
  }

  size_t getBankSubRegionCount(int bankIdx) const {
    return bankPatterns.at(bankIdx).count;
  }

  const AffPatImpl &getAffinePatternImpl(int bankIdx, int patternIdx) const {
    return bankPatterns.at(bankIdx).patterns.at(patternIdx);
  }

  AffinePattern getAffinePattern(int bankIdx, int patternIdx) const {
    const auto &pat = this->getAffinePatternImpl(bankIdx, patternIdx);
    return getAffinePatternFromImpl(pat);
  }

  void clearBankSubRegion(int bankIdx) { bankPatterns.at(bankIdx).count = 0; }

  void addPattern(int bankIdx, const AffPatImpl &pat) {
    auto &b = bankPatterns.at(bankIdx);
    assert(b.count < MaxSubRegions);
    b.patterns[b.count] = pat;
    b.count++;
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
  using Pat1 = LLCTilePatternImpl<1, int64_t, NumBanks>;
  using Pat2 = LLCTilePatternImpl<2, int64_t, NumBanks>;
  using Pat3 = LLCTilePatternImpl<3, int64_t, NumBanks>;
  using PatG = LLCTilePatternGeneric;

  size_t dimension = 0;
  std::shared_ptr<Pat1> pat1;
  std::shared_ptr<Pat2> pat2;
  std::shared_ptr<Pat3> pat3;
  std::shared_ptr<PatG> patG;

  LLCTilePattern()
      : dimension(0), pat1(nullptr), pat2(nullptr), pat3(nullptr),
        patG(nullptr) {}

  LLCTilePattern(const LLCTilePattern &other) : dimension(other.dimension) {
    if (other.pat1) {
      pat1 = std::make_shared<Pat1>(*other.pat1);
    }
    if (other.pat2) {
      pat2 = std::make_shared<Pat2>(*other.pat2);
    }
    if (other.pat3) {
      pat3 = std::make_shared<Pat3>(*other.pat3);
    }
    if (other.patG) {
      patG = std::make_shared<PatG>(*other.patG);
    }
  }

  LLCTilePattern &operator=(const LLCTilePattern &other) {
    this->dimension = other.dimension;
    if (other.pat1) {
      pat1 = std::make_shared<Pat1>(*other.pat1);
    }
    if (other.pat2) {
      pat2 = std::make_shared<Pat2>(*other.pat2);
    }
    if (other.pat3) {
      pat3 = std::make_shared<Pat3>(*other.pat3);
    }
    if (other.patG) {
      patG = std::make_shared<PatG>(*other.patG);
    }
    return *this;
  }

  void initialize(size_t dimension) {

    assert(this->dimension == 0);
    this->dimension = dimension;

#define LLCTilePatternInitCase(dim)                                            \
  case dim: {                                                                  \
    pat##dim = std::make_shared<Pat##dim>();                                   \
    break;                                                                     \
  }

    switch (dimension) {
      LLCTilePatternInitCase(1);
      LLCTilePatternInitCase(2);
      LLCTilePatternInitCase(3);
    default: {
      patG = std::make_shared<PatG>(NumBanks);
      break;
    }
    }

#undef LLCTilePatternInit
  }

  template <size_t dimension>
  LLCTilePatternImpl<dimension, int64_t, NumBanks> &init() {
    // This should never be initialized.
    panic("Should neve intialized.");
  }

#define LLCTilePatternDispRet(func, args...)                                   \
  assert(this->dimension != 0);                                                \
  switch (this->dimension) {                                                   \
  case 1: {                                                                    \
    return this->pat1->func(args);                                             \
  }                                                                            \
  case 2: {                                                                    \
    return this->pat2->func(args);                                             \
  }                                                                            \
  case 3: {                                                                    \
    return this->pat3->func(args);                                             \
  }                                                                            \
  default: {                                                                   \
    return this->patG->func(args);                                             \
  }                                                                            \
  }

#define LLCTilePatternDispVoid(func, args...)                                  \
  assert(this->dimension != 0);                                                \
  switch (this->dimension) {                                                   \
  case 1: {                                                                    \
    this->pat1->func(args);                                                    \
    break;                                                                     \
  }                                                                            \
  case 2: {                                                                    \
    this->pat2->func(args);                                                    \
    break;                                                                     \
  }                                                                            \
  case 3: {                                                                    \
    this->pat3->func(args);                                                    \
    break;                                                                     \
  }                                                                            \
  default: {                                                                   \
    this->patG->func(args);                                                    \
    break;                                                                     \
  }                                                                            \
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
  LLCTilePatternImpl<dim, int64_t, LLCTilePattern::NumBanks>                   \
      &LLCTilePattern::init<dim>();

LLCTilePatternInit(1);
LLCTilePatternInit(2);
LLCTilePatternInit(3);

#undef LLCTilePatternInit

#endif