#include "gem_forge_utils.hh"

#include "base/logging.hh"

#include <sstream>

std::string GemForgeUtils::dataToString(const uint8_t *data, int size) {
  std::stringstream ss;
  ss << "size " << size;
  int highestNonZero = size;
  for (int i = 0; i < size; ++i) {
    if (data[i] != 0) {
      highestNonZero = i;
    }
  }
  if (highestNonZero == size) {
    ss << " AllZero";
  } else {
    for (int i = 0; i <= highestNonZero; ++i) {
      ss << " 0x" << std::hex << static_cast<int>(data[i]);
    }
  }
  return ss.str();
}

uint64_t GemForgeUtils::rebuildData(const uint8_t *data, int size) {
  switch (size) {
  case 8:
    return *reinterpret_cast<const uint64_t *>(data);
  case 4:
    return *reinterpret_cast<const uint32_t *>(data);
  case 2:
    return *reinterpret_cast<const uint16_t *>(data);
  case 1:
    return *reinterpret_cast<const uint8_t *>(data);
  default:
    panic("Unsupported element size %d.\n", size);
  }
}