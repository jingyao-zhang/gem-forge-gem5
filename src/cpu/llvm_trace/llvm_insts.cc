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

bool LLVMDynamicInst::isDependenceReady(LLVMTraceCPU *cpu) const {
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

bool LLVMDynamicInst::canWriteBack(LLVMTraceCPU *cpu) const {
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

LLVMDynamicInstMem::~LLVMDynamicInstMem() {
  if (this->value != nullptr) {
    delete this->value;
    this->value = nullptr;
  }
}

void LLVMDynamicInstMem::execute(LLVMTraceCPU *cpu) {
  switch (this->type) {
  case Type::ALLOCA: {
    // We need to handle stack allocation only
    // when we have a driver.
    if (!cpu->isStandalone()) {
      Addr vaddr = cpu->allocateStack(this->size, this->align);
      // Set up the mapping.
      cpu->mapBaseNameToVAddr(this->new_base, vaddr);
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
    for (const auto &packet : this->packets) {
      cpu->sendRequest(packet.paddr, packet.size, this->id, packet.data);
      DPRINTF(LLVMTraceCPU, "Send request paddr %p size %u for inst %d\n",
              reinterpret_cast<void *>(packet.paddr), packet.size, this->id);
    }
    break;
  }

  default: {
    panic("Unknown LLVMDynamicInstMem type %u\n", this->type);
    break;
  }
  }
}

void LLVMDynamicInstMem::constructPackets(LLVMTraceCPU *cpu) {
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
              inflyPacketsSize, paddr, reinterpret_cast<void *>(paddr));
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
            this->packets.size(), reinterpret_cast<void *>(vaddr),
            reinterpret_cast<void *>(paddr), packetSize, this->id);
  }
}

void LLVMDynamicInstMem::writeback(LLVMTraceCPU *cpu) {
  if (this->type != Type::STORE) {
    panic("Calling writeback on non-store inst %u.\n", this->id);
  }
  // Start to send the packets to cpu for store.
  for (const auto &packet : this->packets) {
    if (packet.size == 8) {
      DPRINTF(LLVMTraceCPU, "Store data %f for inst %u to paddr %p\n",
              *(double *)(packet.data), this->id, packet.paddr);
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

void LLVMDynamicInstMem::handlePacketResponse(LLVMTraceCPU *cpu,
                                              PacketPtr packet) {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic("LLVMDynamicInstMem::handlePacketResponse called for non store/load "
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
