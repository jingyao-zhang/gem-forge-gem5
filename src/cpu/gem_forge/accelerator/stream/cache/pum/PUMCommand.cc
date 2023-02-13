#include "PUMCommand.hh"

#include <iomanip>

namespace gem5 {

std::ostream &operator<<(std::ostream &os, const PUMCommand &command) {
  os << command.to_string();
  return os;
}

std::string PUMCommand::to_string(int llcBankIdx) const {
  std::stringstream os;
  os << "[PUMCmd " << type << " WD-" << wordline_bits << "]\n";
  if (hasReuse()) {
    os << "  Reuse          " << reuse.dim << " x" << reuse.count << "\n";
  }
  if (srcRegion != "none") {
    os << "  Src " << srcRegion << " Acc " << srcAccessPattern << " Map "
       << srcMapPattern << '\n';
  }
  if (dstRegion != "none") {
    os << "  Dst " << dstRegion << " Acc " << dstAccessPattern << " Map "
       << dstMapPattern << '\n';
  }
  os << "  BitlineMask    " << bitline_mask << " ";
  os << "TileMask       " << tile_mask << '\n';
  if (type == "intra-array") {
    os << "  BitlineDist    " << bitline_dist << '\n';
  } else if (type == "inter-array") {
    os << "  TileDist       " << tile_dist << '\n';
    os << "  DstBitlineMask " << dst_bitline_mask << '\n';
    for (auto i = 0; i < inter_array_splits.size(); ++i) {
      os << "    InterArraySplit " << std::setw(2) << i << '\n';
      const auto &patterns = inter_array_splits[i];
      for (auto j = 0; j < patterns.size(); ++j) {
        os << "      " << getAffinePatternFromImpl(patterns[j]) << '\n';
      }
    }
  } else {
    // Compute command.
    os << "  Op " << enums::OpClassStrings[opClass]
       << (isReduction ? " [Reduce] " : "") << '\n';
  }
  std::vector<int> mappedLLCBanks;
  for (auto i = 0; i < llcSplitTileCmds.NumBanks; ++i) {
    if (llcBankIdx != -1 && i != llcBankIdx) {
      continue;
    }
    auto subRegionCount = llcSplitTileCmds.getBankSubRegionCount(i);
    if (subRegionCount > 0) {
      mappedLLCBanks.push_back(i);
    }
  }
  int dumpLLCFirst = 2;
  int dumpLLCLast = 2;
  bool dumpedSkip = false;
  for (auto idx = 0; idx < mappedLLCBanks.size(); ++idx) {
    if (idx >= dumpLLCFirst && idx + dumpLLCLast <= mappedLLCBanks.size()) {
      if (!dumpedSkip) {
        os << "    Skip " << mappedLLCBanks.size() - dumpLLCFirst - dumpLLCLast;
        dumpedSkip = true;
      }
      continue;
    }
    auto i = mappedLLCBanks.at(idx);
    auto subRegionCount = llcSplitTileCmds.getBankSubRegionCount(i);
    if (subRegionCount > 0) {
      os << "    LLCCmd " << std::setw(2) << i;
      for (auto j = 0; j < subRegionCount; ++j) {
        os << "  " << llcSplitTileCmds.getAffinePattern(i, j);
        if (type == "inter-array" && hasReuse()) {

          const auto &dstTilePat = llcSplitDstTileCmds[i][j];

          os << " ->\n";
          os << "        DstBank ";

          for (auto dstBankIdx = 0; dstBankIdx < dstTilePat.size();
               ++dstBankIdx) {
            if (dstTilePat.at(dstBankIdx)) {
              os << dstBankIdx << " ";
            }
          }
          os << "\n";
        } else {
          os << "\n";
        }
      }
    }
  }
  return os.str();
}
} // namespace gem5
