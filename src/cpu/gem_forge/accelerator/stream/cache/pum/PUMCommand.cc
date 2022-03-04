#include "PUMCommand.hh"

#include <iomanip>

std::ostream &operator<<(std::ostream &os, const PUMCommand &command) {
  os << "[PUMCmd " << command.type << " WD-" << command.wordline_bits << "]\n";
  if (command.srcRegion != "none") {
    os << "  Src " << command.srcRegion << " Acc " << command.srcAccessPattern
       << " Map " << command.srcMapPattern << '\n';
  }
  if (command.dstRegion != "none") {
    os << "  Dst " << command.dstRegion << " Acc " << command.dstAccessPattern
       << " Map " << command.dstMapPattern << '\n';
  }
  os << "  BitlineMask    " << command.bitline_mask << '\n';
  os << "  TileMask       " << command.tile_mask << '\n';
  if (command.type == "intra-array") {
    os << "  BitlineDist    " << command.bitline_dist << '\n';
  } else {
    os << "  TileDist       " << command.tile_dist << '\n';
    os << "  SrcBitlineMask " << command.src_bitline_mask << '\n';
    os << "  DstBitlineMask " << command.dst_bitline_mask << '\n';
    for (auto i = 0; i < command.inter_array_splits.size(); ++i) {
      os << "    InterArraySplit " << std::setw(2) << i << '\n';
      const auto &patterns = command.inter_array_splits[i];
      for (auto j = 0; j < patterns.size(); ++j) {
        os << "      " << patterns[j] << '\n';
      }
    }
  }
  for (auto i = 0; i < command.llc_commands.size(); ++i) {
    const auto &patterns = command.llc_commands[i];
    if (!patterns.empty()) {
      os << "    LLCCmd " << std::setw(2) << i;
      for (auto j = 0; j < patterns.size(); ++j) {
        os << "  " << patterns[j];
      }
      os << '\n';
    }
  }
  return os;
}

std::string PUMCommand::to_string() const {
  std::stringstream os;
  os << *this;
  return os.str();
}