#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_accelerator.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

std::unordered_map<std::string, OpClass> LLVMDynamicInst::instToOpClass = {
    // Binary operator.
    {"add", IntAluOp},
    {"fadd", FloatAddOp},
    {"sub", IntAluOp},
    {"fsub", FloatAddOp},
    {"mul", IntMultOp},
    {"fmul", FloatMultOp},
    {"udiv", IntDivOp},
    {"sdiv", IntDivOp},
    {"fdiv", FloatDivOp},
    {"urem", IntDivOp},
    {"srem", IntDivOp},
    {"frem", FloatDivOp},
    // Bitwise binary operator.
    {"shl", IntAluOp},
    {"lshr", IntAluOp},
    {"ashr", IntAluOp},
    {"and", IntAluOp},
    {"or", IntAluOp},
    {"xor", IntAluOp},
    // Conversion operator.
    // Truncation requires no FU.
    {"trunc", Enums::No_OpClass},
    {"zext", IntAluOp},
    {"sext", IntAluOp},
    {"fptrunc", FloatCvtOp},
    {"fpext", FloatCvtOp},
    {"fptoui", FloatCvtOp},
    {"fptosi", FloatCvtOp},
    {"uitofp", FloatCvtOp},
    {"sitofp", FloatCvtOp},
    {"ptrtoint", Enums::No_OpClass},
    {"inttoptr", Enums::No_OpClass},
    {"bitcast", Enums::No_OpClass},
    // Other insts.
    {"icmp", IntAluOp},
    {"fcmp", FloatCmpOp},
    // We assume the branching requires address computation.
    {"br", IntAluOp},
    // Our special accelerator inst.
    {"cca", Enums::OpClass::Accelerator},
    {"load", Enums::OpClass::MemRead},
    {"store", Enums::OpClass::MemWrite},
};

void LLVMDynamicInst::handleFUCompletion() {
  if (this->fuStatus != FUStatus::WORKING) {
    panic("fuStatus should be working when a FU completes, instead %d\n",
          this->fuStatus);
  }
  this->fuStatus = FUStatus::COMPLETE_NEXT_CYCLE;
}

bool LLVMDynamicInst::isBranchInst() const {
  return this->instName == "br" || this->instName == "ret" ||
         this->instName == "call";
}

bool LLVMDynamicInst::isDependenceReady(LLVMTraceCPU* cpu) const {
  for (const auto dependentInstId : this->dependentInstIds) {
    auto DepInst = cpu->getDynamicInst(dependentInstId);
    // Special case for store, as it should not be speculated.
    if (DepInst->isBranchInst()) {
      continue;
      // // This is control dependence, only used for store.
      // if (this->instName == "store") {
      //   // Make sure the control dependence is committed.
      //   if (!cpu->isInstCommitted(dependentInstId)) {
      //     return false;
      //   }
      // } else {
      //   // Ignore control dependence for speculation.
      //   continue;
      // }
    } else {
      // Register or mem dependence, check if
      // write backed.
      if (!cpu->isInstFinished(dependentInstId)) {
        return false;
      }
    }
  }
  return true;
}

// For now just return IntAlu.
OpClass LLVMDynamicInst::getOpClass() const {
  auto iter = LLVMDynamicInst::instToOpClass.find(this->instName);
  if (iter == LLVMDynamicInst::instToOpClass.end()) {
    // For unknown, simply return IntAlu.
    return No_OpClass;
  }
  return iter->second;
}

void LLVMDynamicInst::startFUStatusFSM() {
  if (this->fuStatus != FUStatus::COMPLETED) {
    panic(
        "fuStatus should be initialized in COMPLETED before starting, instead "
        "of %d\n",
        this->fuStatus);
  }
  if (this->getOpClass() != No_OpClass) {
    this->fuStatus = FUStatus::WORKING;
  }
}

bool LLVMDynamicInst::canWriteBack(LLVMTraceCPU* cpu) const {
  if (this->instName == "store") {
    for (const auto dependentInstId : this->dependentInstIds) {
      auto DepInst = cpu->getDynamicInst(dependentInstId);
      // Special case for store, as it should not be speculated.
      if (DepInst->isBranchInst()) {
        // Make sure the control dependence is committed.
        if (!cpu->isInstCommitted(dependentInstId)) {
          return false;
        }
      }
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
          // No address translation.
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
        // For now only support maximum 8 bytes access.
        packetSize = this->size - inflyPacketsSize;
        if (packetSize > 8) {
          packetSize = 8;
        }
        // Do not span across cache line.
        auto cacheLineSize = cpu->system->cacheLineSize();
        if (((paddr % cacheLineSize) + packetSize) > cacheLineSize) {
          packetSize = cacheLineSize - (paddr % cacheLineSize);
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
  size_t idx = 0, prev = 0;
  for (; idx < source.size(); ++idx) {
    if (source[idx] == split) {
      ret.push_back(source.substr(prev, idx - prev));
      prev = idx + 1;
    }
  }
  // Don't miss the possible last field.
  if (prev < idx) {
    ret.push_back(source.substr(prev, idx - prev));
  }
  return std::move(ret);
}

// Return a newed buffer of the store value.
uint8_t* extractStoreValue(int typeId, Addr size, const std::string& typeName,
                           const std::string& content) {
  uint8_t* value;
  switch (typeId) {
    case 2: {
      // float type.
      value = (uint8_t*)(new float);
      *((float*)value) = stof(content);
      break;
    }
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

std::vector<LLVMDynamicInstId> extractDependentInsts(const std::string deps) {
  std::vector<LLVMDynamicInstId> dependentInstIds;
  for (const std::string& dependentIdStr : splitByChar(deps, ',')) {
    dependentInstIds.push_back(stoull(dependentIdStr));
  }
  return std::move(dependentInstIds);
}
}  // namespace

std::shared_ptr<LLVMDynamicInst> parseLLVMDynamicInst(LLVMDynamicInstId id,
                                                      const std::string& line) {
  const size_t NAME_FIELD = 0;
  const size_t NUM_MICRO_OPS_FIELD = NAME_FIELD + 1;
  const size_t DEPENDENT_INST_ID_FIELD = NUM_MICRO_OPS_FIELD + 1;

  auto fields = splitByChar(line, '|');
  const std::string& instName = fields[NAME_FIELD];
  auto numMicroOps = stoul(fields[NUM_MICRO_OPS_FIELD]);
  auto dependentInstIds =
      extractDependentInsts(fields[DEPENDENT_INST_ID_FIELD]);

  if (instName == "store") {
    auto type = LLVMDynamicInstMem::Type::STORE;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = stoull(fields[DEPENDENT_INST_ID_FIELD + 2]);
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 3]);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 4]);
    // Handle the value of store operation.
    int typeId = stoi(fields[DEPENDENT_INST_ID_FIELD + 5]);
    uint8_t* value =
        extractStoreValue(typeId, size, fields[DEPENDENT_INST_ID_FIELD + 6],
                          fields[DEPENDENT_INST_ID_FIELD + 7]);
    return std::make_shared<LLVMDynamicInstMem>(
        id, instName, numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value);
    // return std::shared_ptr<LLVMDynamicInst>(new LLVMDynamicInstMem(
    //     id, instName, numMicroOps, std::move(dependentInstIds), size, base,
    //     offset, trace_vaddr, 16, type, value));
  } else if (instName == "load") {
    auto type = LLVMDynamicInstMem::Type::LOAD;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = stoull(fields[DEPENDENT_INST_ID_FIELD + 2]);
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 3]);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 4]);
    uint8_t* value = nullptr;
    return std::make_shared<LLVMDynamicInstMem>(
        id, instName, numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value);
  } else if (instName == "alloca") {
    auto type = LLVMDynamicInstMem::Type::ALLOCA;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = 0;
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 2]);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 3]);
    uint8_t* value = nullptr;
    return std::make_shared<LLVMDynamicInstMem>(
        id, instName, numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value);
  } else {
    auto type = LLVMDynamicInstCompute::Type::OTHER;
    LLVMAcceleratorContext* context = nullptr;
    if (instName == "call") {
      type = LLVMDynamicInstCompute::Type::CALL;
    } else if (instName == "ret") {
      type = LLVMDynamicInstCompute::Type::RET;
    } else if (instName == "sin") {
      type = LLVMDynamicInstCompute::Type::SIN;
    } else if (instName == "cos") {
      type = LLVMDynamicInstCompute::Type::COS;
    } else if (instName == "cca") {
      type = LLVMDynamicInstCompute::Type::ACCELERATOR;
      if (fields.size() != 3) {
        panic("Missing context field for accelerator inst %u.\n", id);
      }
      context = LLVMAcceleratorContext::parseContext(fields[2]);
    }
    return std::make_shared<LLVMDynamicInstCompute>(
        id, instName, numMicroOps, std::move(dependentInstIds), type, context);
  }

  panic("Unknown type of LLVMDynamicInst %s.\n", instName.c_str());
  return std::shared_ptr<LLVMDynamicInst>();
}
