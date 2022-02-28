#include "PUMCommand.hh"

#include <iomanip>

std::ostream &operator<<(std::ostream &os, const PUMCommand &command) {
  os << "[PUMCmd " << command.type << "]\n";
  os << "  BitlineMask    " << command.bitline_mask << '\n';
  os << "  SrcBitlineMask " << command.src_bitline_mask << '\n';
  os << "  DstBitlineMask " << command.dst_bitline_mask << '\n';
  os << "  TileMask       " << command.tile_mask << '\n';
  os << "  TileDist       " << command.tile_dist << '\n';
  for (auto i = 0; i < command.inter_array_splits.size(); ++i) {
    os << "    InterArraySplit " << std::setw(2) << i << '\n';
    const auto &patterns = command.inter_array_splits[i];
    for (auto j = 0; j < patterns.size(); ++j) {
      os << "      " << patterns[j] << '\n';
    }
  }
  for (auto i = 0; i < command.llc_commands.size(); ++i) {
    os << "    LLCCmd " << std::setw(2) << i << '\n';
    const auto &patterns = command.llc_commands[i];
    for (auto j = 0; j < patterns.size(); ++j) {
      os << "      " << patterns[j] << '\n';
    }
  }
  return os;
}

std::string PUMCommand::to_string() const {
  std::stringstream os;
  os << *this;
  return os.str();
}