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

bool LLVMDynamicInst::isConditionalBranchInst() const {
  return (this->instName == "br" || this->instName == "switch") &&
         this->staticInstAddress != 0x0 && this->nextBBName != "";
}

bool LLVMDynamicInst::isBranchInst() const {
  return this->instName == "br" || this->instName == "switch" ||
         this->instName == "ret" || this->instName == "call";
}

bool LLVMDynamicInst::isStoreInst() const { return this->instName == "store"; }

bool LLVMDynamicInst::isLoadInst() const { return this->instName == "load"; }

bool LLVMDynamicInst::isDependenceReady(LLVMTraceCPU* cpu) const {
  for (const auto dependentInstId : this->dependentInstIds) {
    if (!cpu->isInstFinished(dependentInstId)) {
      // We should not be blocked by control dependence here.
      auto DepInst = cpu->getInflyInst(dependentInstId);
      if (DepInst->isBranchInst()) {
        continue;
      }
      return false;
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
      if (!cpu->isInstCommitted(dependentInstId)) {
        // If the dependent inst is not committed, check if it's a branch.
        auto DepInst = cpu->getInflyInst(dependentInstId);
        if (DepInst->isBranchInst()) {
          return false;
        }
      }
    }
  }
  return true;
}

void LLVMDynamicInstMem::execute(LLVMTraceCPU* cpu) {
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
    case Type::STORE: {
      // Just construct the packets.
      this->constructPackets(cpu);
      break;
    }
    case Type::LOAD: {
      this->constructPackets(cpu);
      for (const auto& packet : this->packets) {
        cpu->sendRequest(packet.paddr, packet.size, this->id, packet.data);
        DPRINTF(LLVMTraceCPU, "Send request paddr %p size %u for inst %d\n",
                reinterpret_cast<void*>(packet.paddr), packet.size, this->id);
      }
      break;
    }

    default: {
      panic("Unknown LLVMDynamicInstMem type %u\n", this->type);
      break;
    }
  }
}

void LLVMDynamicInstMem::constructPackets(LLVMTraceCPU* cpu) {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic("Calling writeback on non-store/load inst %u.\n", this->id);
  }

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
      vaddr =
          cpu->getVAddrFromBase(this->base) + this->offset + inflyPacketsSize;
      paddr = cpu->getPAddrFromVaddr(vaddr);
      DPRINTF(LLVMTraceCPU,
              "vaddr %llu = base %llu + offset %llu + infly %llu, paddr %llu, "
              "reintered %p\n",
              vaddr, cpu->getVAddrFromBase(this->base), this->offset,
              inflyPacketsSize, paddr, reinterpret_cast<void*>(paddr));
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

    // Construct the packet.
    if (this->type == Type::LOAD) {
      this->packets.emplace_back(paddr, packetSize, nullptr);
    } else {
      this->packets.emplace_back(paddr, packetSize,
                                 this->value + inflyPacketsSize);
    }

    DPRINTF(LLVMTraceCPU,
            "Construct request %d vaddr %p paddr %p size %u for inst %d\n",
            this->packets.size(), reinterpret_cast<void*>(vaddr),
            reinterpret_cast<void*>(paddr), packetSize, this->id);
  }
}

void LLVMDynamicInstMem::writeback(LLVMTraceCPU* cpu) {
  if (this->type != Type::STORE) {
    panic("Calling writeback on non-store inst %u.\n", this->id);
  }
  // Start to send the packets to cpu for store.
  for (const auto& packet : this->packets) {
    if (packet.size == 8) {
      DPRINTF(LLVMTraceCPU, "Store data %f for inst %u to paddr %p\n",
              *(double*)(packet.data), this->id, packet.paddr);
    }
    cpu->sendRequest(packet.paddr, packet.size, this->id, packet.data);
  }
}

bool LLVMDynamicInstMem::isCompleted() const {
  if (this->type != Type::STORE) {
    return this->fuStatus == FUStatus::COMPLETED && this->packets.size() == 0 &&
           this->remainingMicroOps == 0;
  } else {
    // For store, the infly packets doesn't control completeness.
    return this->fuStatus == FUStatus::COMPLETED &&
           this->remainingMicroOps == 0;
  }
}

bool LLVMDynamicInstMem::isWritebacked() const {
  if (this->type != Type::STORE) {
    panic("Calling isWritebacked on non-store inst %u.\n", this->id);
  }
  return this->packets.size() == 0;
}

void LLVMDynamicInstMem::handlePacketResponse(LLVMTraceCPU* cpu,
                                              PacketPtr packet) {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic(
        "LLVMDynamicInstMem::handlePacketResponse called for non store/load "
        "inst %d, but type %d.\n",
        this->id, this->type);
  }

  // Check if the load will produce a new base.
  if (this->type == Type::LOAD && this->new_base != "") {
    uint64_t vaddr = packet->get<uint64_t>();
    cpu->mapBaseNameToVAddr(this->new_base, vaddr);
  }

  // We don't care about which packet for now, just mark one completed.
  this->packets.pop_front();
  DPRINTF(LLVMTraceCPU, "Get response for inst %u, remain infly packets %d\n",
          this->id, this->packets.size());
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

std::shared_ptr<LLVMDynamicInst> parseLLVMDynamicInst(const std::string& line) {
  const size_t NAME_FIELD = 0;
  const size_t ID_FIELD = NAME_FIELD + 1;
  const size_t NUM_MICRO_OPS_FIELD = NAME_FIELD + 1;
  const size_t DEPENDENT_INST_ID_FIELD = NUM_MICRO_OPS_FIELD + 1;

  auto fields = splitByChar(line, '|');
  const std::string& instName = fields[NAME_FIELD];
  int numMicroOps = 1;
  LLVMDynamicInstId id = stoul(fields[ID_FIELD]);
  auto dependentInstIds =
      extractDependentInsts(fields[DEPENDENT_INST_ID_FIELD]);

  if (instName == "store") {
    auto type = LLVMDynamicInstMem::Type::STORE;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = stoull(fields[DEPENDENT_INST_ID_FIELD + 2]);
    DPRINTF(LLVMTraceCPU, "OFFSET %s %llu\n",
            fields[DEPENDENT_INST_ID_FIELD + 2].c_str(), offset);
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 3], 0, 16);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 4]);
    // Handle the value of store operation.
    int typeId = stoi(fields[DEPENDENT_INST_ID_FIELD + 5]);
    uint8_t* value =
        extractStoreValue(typeId, size, fields[DEPENDENT_INST_ID_FIELD + 6],
                          fields[DEPENDENT_INST_ID_FIELD + 7]);
    std::string new_base = "";
    return std::make_shared<LLVMDynamicInstMem>(
        id, instName, numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value, new_base);
  } else if (instName == "load") {
    auto type = LLVMDynamicInstMem::Type::LOAD;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = stoull(fields[DEPENDENT_INST_ID_FIELD + 2]);
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 3], 0, 16);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 4]);
    uint8_t* value = nullptr;
    std::string new_base = fields[DEPENDENT_INST_ID_FIELD + 5];
    return std::make_shared<LLVMDynamicInstMem>(
        id, instName, numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value, new_base);
  } else if (instName == "alloca") {
    auto type = LLVMDynamicInstMem::Type::ALLOCA;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = 0;
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 2], 0, 16);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 3]);
    uint8_t* value = nullptr;
    std::string new_base = "";
    return std::make_shared<LLVMDynamicInstMem>(
        id, instName, numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value, new_base);
  } else if (instName == "memset") {
    // Translate this into a big store.
    auto type = LLVMDynamicInstMem::Type::STORE;
    auto base = fields[DEPENDENT_INST_ID_FIELD + 1];
    Addr offset = stoull(fields[DEPENDENT_INST_ID_FIELD + 2]);
    DPRINTF(LLVMTraceCPU, "OFFSET %s %llu\n",
            fields[DEPENDENT_INST_ID_FIELD + 2].c_str(), offset);
    Addr trace_vaddr = stoull(fields[DEPENDENT_INST_ID_FIELD + 3], 0, 16);
    Addr size = stoull(fields[DEPENDENT_INST_ID_FIELD + 4]);
    uint8_t* value = new uint8_t[size];
    uint8_t ch = stoul(fields[DEPENDENT_INST_ID_FIELD + 5]);
    std::memset(value, ch, size);
    std::string new_base = "";
    return std::make_shared<LLVMDynamicInstMem>(
        id, "store", numMicroOps, std::move(dependentInstIds), size, base,
        offset, trace_vaddr, 16, type, value, new_base);
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
      // For now just use one cycle latency for cca.
      context = LLVMAcceleratorContext::parseContext("1");
      // if (fields.size() != 4) {
      //   panic("Missing context field for accelerator inst %u.\n", id);
      // }
      // context = LLVMAcceleratorContext::parseContext(fields[3]);
    } else if ((instName == "br" || instName == "switch") &&
               fields.size() == 5) {
      // This is a conditional branch.
      uint64_t staticInstAddress =
          stoull(fields[DEPENDENT_INST_ID_FIELD + 1], 0, 16);
      auto dynamicInst = std::make_shared<LLVMDynamicInstCompute>(
          id, instName, numMicroOps, std::move(dependentInstIds), type,
          context);
      // Set the inst address and target basic block.
      dynamicInst->setStaticInstAddress(staticInstAddress);
      dynamicInst->setNextBBName(fields[DEPENDENT_INST_ID_FIELD + 2]);
      DPRINTF(LLVMTraceCPU, "Parsed a conditional branch %p, %s.\n",
              reinterpret_cast<void*>(staticInstAddress),
              dynamicInst->getNextBBName().c_str());
      return dynamicInst;
    }
    return std::make_shared<LLVMDynamicInstCompute>(
        id, instName, numMicroOps, std::move(dependentInstIds), type, context);
  }

  panic("Unknown type of LLVMDynamicInst %s.\n", instName.c_str());
  return std::shared_ptr<LLVMDynamicInst>();
}
