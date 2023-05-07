#include "gem_forge_utils.hh"

#include "base/logging.hh"

#include <sstream>

namespace gem5 {

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

int GemForgeUtils::translateToNumRegs(const DataType &type) {
  auto numRegs = 1;
  switch (type) {
  case DataType::FLOAT:
  case DataType::DOUBLE:
    numRegs = 1;
    break;
  case DataType::VECTOR_128:
    numRegs = 2;
    break;
  case DataType::VECTOR_256:
    numRegs = 4;
    break;
  case DataType::VECTOR_512:
    numRegs = 8;
    break;
  case DataType::VOID:
    numRegs = 0;
    break;
  default:
    panic("Invalid data type.");
  }
  return numRegs;
}

std::string GemForgeUtils::printTypedData(const DataType &type,
                                          const uint64_t *data) {
  switch (type) {
  case DataType::INTEGER:
  case DataType::INT1: {
    return csprintf("%#x", data[0]);
  }
  case DataType::INT32_INT1: {
    auto v32 = data[0] & 0xFFFFFFFF;
    auto v1 = data[0] >> 32;
    return csprintf("{%#x, %#x}", v32, v1);
  }
  case DataType::FLOAT: {
    return csprintf("%f", *reinterpret_cast<const float *>(data));
  }
  case DataType::DOUBLE: {
    return csprintf("%lf", *reinterpret_cast<const double *>(data));
  }
  default: {
    auto numRegs = translateToNumRegs(type);
    return GemForgeUtils::dataToString(reinterpret_cast<const uint8_t *>(data),
                                       numRegs * 8);
  }
  }
}

} // namespace gem5
