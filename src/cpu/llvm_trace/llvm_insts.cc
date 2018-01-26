#include "cpu/llvm_trace/llvm_insts.hh"

#include "base/misc.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

bool LLVMDynamicInst::isDependenceReady(LLVMTraceCPU* cpu) const {
  for (const auto dependentInstId : this->dependentInstIds) {
    if (!cpu->isInstFinished(dependentInstId)) {
      return false;
    }
  }
  return true;
}

void LLVMDynamicInstMem::execute(LLVMTraceCPU* cpu) {
  this->numInflyPackets = 0;
  switch (this->type) {
    case Type::ALLOCA: {
      // We need to handle stack allocation only
      // when we have a driver.
      if (!cpu->isStandalone()) {
        Addr vaddr = cpu->allocateStack(this->size, this->align);
        // Set up the mapping.
        cpu->mapBaseNameToVAddr(this->base, vaddr);
      }
      break;
    }
    case Type::LOAD:
    case Type::STORE: {
      for (int packetSize, inflyPacketsSize = 0; inflyPacketsSize < this->size;
           inflyPacketsSize += packetSize) {
        Addr paddr, vaddr;
        if (cpu->isStandalone()) {
          // When in stand alone mode, use the trace space address
          // directly as the virtual address.
          vaddr = this->trace_vaddr + inflyPacketsSize;
          paddr = cpu->translateAndAllocatePhysMem(vaddr);
        } else {
          // When we have a driver, we have to translate trace space
          // address into simulation space and then use the process
          // page table to get physical address.
          vaddr = cpu->getVAddrFromBase(this->base) + this->offset +
                  inflyPacketsSize;
          paddr = cpu->getPAddrFromVaddr(vaddr);
        }
        // For now only support maximum 16 bytes access.
        packetSize = this->size - inflyPacketsSize;
        if (packetSize > 16) {
          packetSize = 16;
        }

        // Send the packet.
        if (this->type == Type::LOAD) {
          cpu->sendRequest(paddr, packetSize, this->id, nullptr);
        } else {
          cpu->sendRequest(paddr, packetSize, this->id,
                           this->value + inflyPacketsSize);
        }

        DPRINTF(LLVMTraceCPU,
                "Send request %d vaddr %p paddr %p size %u for inst %d\n",
                this->numInflyPackets, reinterpret_cast<void*>(vaddr),
                reinterpret_cast<void*>(paddr), packetSize, this->id);

        // Update infly packets number.
        this->numInflyPackets++;
      }
      break;
    }
    default: {
      panic("Unknown LLVMDynamicInstMem type %u\n", this->type);
      break;
    }
  }
}

void LLVMDynamicInstMem::handlePacketResponse() {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic(
        "LLVMDynamicInstMem::handlePacketResponse called for non store/load "
        "inst %d, but type %d.\n",
        this->id, this->type);
  }
  this->numInflyPackets--;
  DPRINTF(LLVMTraceCPU, "Get response for inst %u, remain infly packets %d\n",
          this->id, this->numInflyPackets);
}

namespace {
/**
 * Split a string like a|b|c| into [a, b, c].
 */
std::vector<std::string> splitByChar(const std::string& source, char split) {
  std::vector<std::string> ret;
  for (size_t idx = 0, prev = 0; idx < source.size(); ++idx) {
    if (source[idx] == split) {
      ret.push_back(source.substr(prev, idx - prev));
      prev = idx + 1;
    }
  }
  return std::move(ret);
}

// Return a newed buffer of the store value.
uint8_t* extractStoreValue(int typeId, Addr size, const std::string& typeName,
                           const std::string& content) {
  uint8_t* value;
  switch (typeId) {
    case 3: {
      // Double type.
      value = (uint8_t*)(new double);
      *((double*)value) = stod(content);
      break;
    }
    case 11: {
      // Arbitrary bit width integer. Check the type name.
      if (typeName == "i64") {
        value = (uint8_t*)(new uint64_t);
        *((uint64_t*)value) = stoull(content);
      } else if (typeName == "i32") {
        value = (uint8_t*)(new uint32_t);
        *((uint32_t*)value) = stoul(content);
      } else if (typeName == "i8") {
        value = new uint8_t;
        *((uint8_t*)value) = stoul(content);
      } else {
        fatal("Unsupported integer type %s\n", typeName.c_str());
      }
      break;
    }
    case 16: {
      // Vector.
      uint8_t* buffer = new uint8_t[size];
      size_t idx = 0;
      for (auto b : splitByChar(content, ',')) {
        if (idx >= size) {
          fatal(
              "Number of bytes exceeds the size %u of the vector, content "
              "%s\n",
              size, content.c_str());
        }
        // Parse the vector.
        buffer[idx++] = (uint8_t)(stoul(b) & 0xFF);
      }
      if (idx != size) {
        fatal("Number of bytes not equal to the size %u, content %s\n", size,
              content.c_str());
      }
      value = buffer;
      break;
    }
    default:
      fatal("Unsupported type id %d\n", typeId);
  }
  return value;
}

std::vector<LLVMDynamicInstId> extractDependentInsts(
    const std::vector<std::string>& fields, int idx) {
  std::vector<LLVMDynamicInstId> dependentInstIds;
  while (idx < fields.size()) {
    if (fields[idx] != "") {
      // Empty string may happen when there is no dependence.
      dependentInstIds.push_back(std::stoull(fields[idx]));
    }
    idx++;
  }
  return std::move(dependentInstIds);
}
}  // namespace

std::shared_ptr<LLVMDynamicInst> parseLLVMDynamicInst(LLVMDynamicInstId id,
                                                      const std::string& line) {
  auto fields = splitByChar(line, '|');
  if (fields[0] == "s") {
    auto type = LLVMDynamicInstMem::Type::STORE;
    auto base = fields[2];
    Addr offset = stoull(fields[3]);
    Addr trace_vaddr = stoull(fields[4]);
    Addr size = stoull(fields[5]);
    // Handle the value of store operation.
    int typeId = stoi(fields[6]);
    uint8_t* value = extractStoreValue(typeId, size, fields[7], fields[8]);
    auto dependentInstIds = extractDependentInsts(fields, 9);
    return std::shared_ptr<LLVMDynamicInst>(
        new LLVMDynamicInstMem(id, std::move(dependentInstIds), size, base,
                               offset, trace_vaddr, 16, type, value));
  } else if (fields[0] == "l") {
    auto type = LLVMDynamicInstMem::Type::LOAD;
    auto base = fields[2];
    Addr offset = stoull(fields[3]);
    Addr trace_vaddr = stoull(fields[4]);
    Addr size = stoull(fields[5]);
    uint8_t* value = nullptr;
    auto dependentInstIds = extractDependentInsts(fields, 6);
    return std::shared_ptr<LLVMDynamicInst>(
        new LLVMDynamicInstMem(id, std::move(dependentInstIds), size, base,
                               offset, trace_vaddr, 16, type, value));
  } else if (fields[0] == "alloca") {
    auto type = LLVMDynamicInstMem::Type::ALLOCA;
    auto base = fields[2];
    Addr offset = 0;
    Addr trace_vaddr = stoull(fields[3]);
    Addr size = stoull(fields[4]);
    uint8_t* value = nullptr;
    auto dependentInstIds = extractDependentInsts(fields, 6);
    return std::shared_ptr<LLVMDynamicInst>(
        new LLVMDynamicInstMem(id, std::move(dependentInstIds), size, base,
                               offset, trace_vaddr, 16, type, value));
  } else {
    auto type = LLVMDynamicInstCompute::Type::OTHER;
    if (fields[0] == "call") {
      type = LLVMDynamicInstCompute::Type::CALL;
    } else if (fields[0] == "ret") {
      type = LLVMDynamicInstCompute::Type::RET;
    } else if (fields[0] == "sin") {
      type = LLVMDynamicInstCompute::Type::SIN;
    } else if (fields[0] == "cos") {
      type = LLVMDynamicInstCompute::Type::COS;
    }
    Tick computeDelay = std::stoul(fields[1]);
    auto dependentInstIds = extractDependentInsts(fields, 2);
    return std::shared_ptr<LLVMDynamicInst>(new LLVMDynamicInstCompute(
        id, std::move(dependentInstIds), computeDelay, type));
  }

  panic("Unknown type of LLVMDynamicInst %s.\n", fields[0].c_str());
  return std::shared_ptr<LLVMDynamicInst>();
}
