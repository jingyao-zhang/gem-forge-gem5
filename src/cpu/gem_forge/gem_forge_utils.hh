#ifndef __CPU_GEM_FORGE_UTILS_HH__
#define __CPU_GEM_FORGE_UTILS_HH__

#include <cstdint>
#include <string>

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

namespace gem5 {

class GemForgeUtils {
public:
  static std::string dataToString(const uint8_t *data, int size);
  static uint64_t rebuildData(const uint8_t *data, int size);

  using DataType = ::LLVM::TDG::DataType;

  static int translateToNumRegs(const DataType &type);
  static std::string printTypedData(const DataType &type, const uint64_t *data);
};

} // namespace gem5

#endif