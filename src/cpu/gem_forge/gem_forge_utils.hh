#ifndef __CPU_GEM_FORGE_UTILS_HH__
#define __CPU_GEM_FORGE_UTILS_HH__

#include <cstdint>
#include <string>

class GemForgeUtils {
public:
  static std::string dataToString(const uint8_t *data, int size);
  static uint64_t rebuildData(const uint8_t *data, int size);
};

#endif