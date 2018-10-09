#include "cpu/llvm_trace/llvm_insts.hh"
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

uint64_t LLVMDynamicInst::currentSeqNum = 0;
uint64_t LLVMDynamicInst::allocateSeqNum() {
  // 0 is reserved for invalid seq num.
  return ++LLVMDynamicInst::currentSeqNum;
}

uint64_t LLVMDynamicInst::getStaticInstAddress() const {
  if (!this->isConditionalBranchInst()) {
    panic("getNextBBName called on non conditional branch instructions.");
  }
  return this->TDG.branch().static_id();
}

const std::string &LLVMDynamicInst::getNextBBName() const {
  if (!this->isConditionalBranchInst()) {
    panic("getNextBBName called on non conditional branch instructions.");
  }
  return this->TDG.branch().next_bb();
}

bool LLVMDynamicInst::isConditionalBranchInst() const {
  const auto &op = this->getInstName();
  return (op == "br" || op == "switch") && this->TDG.has_branch();
}

bool LLVMDynamicInst::isBranchInst() const {
  const auto &op = this->getInstName();
  return op == "br" || op == "switch" || op == "ret" || op == "call";
}

bool LLVMDynamicInst::isStoreInst() const {
  const auto &op = this->getInstName();
  return op == "store" || op == "memset";
}

bool LLVMDynamicInst::isLoadInst() const {
  return this->getInstName() == "load";
}

bool LLVMDynamicInst::isDependenceReady(LLVMTraceCPU *cpu) const {
  for (const auto dependentInstId : this->TDG.deps()) {
    if (!cpu->isInstFinished(dependentInstId)) {
      // We should not be blocked by control dependence here.
      auto depInst = cpu->getInflyInst(dependentInstId);
      if (depInst->isBranchInst()) {
        continue;
      }
      return false;
    }
  }

  // Check the stream engine.
  for (const auto &streamId : this->TDG.used_stream_ids()) {
    if (!cpu->getAcceleratorManager()->isStreamReady(streamId, this)) {
      return false;
    }
  }
  return true;
}

void LLVMDynamicInst::dumpBasic() const {
  inform("Dump Inst seq %lu, id %lu, op %s.\n", this->seqNum, this->getId(),
         this->getInstName().c_str());
}

void LLVMDynamicInst::dumpDeps(LLVMTraceCPU *cpu) const {
  this->dumpBasic();
  inform("Deps Begin ===========================\n");
  for (const auto dependentInstId : this->TDG.deps()) {
    auto depInst = cpu->getInflyInstNullable(dependentInstId);
    if (depInst == nullptr) {
      inform("Dep id %lu nullptr\n", dependentInstId);
    } else {
      depInst->dumpBasic();
    }
  }
  inform("Deps End   ===========================\n");
}

// For now just return IntAlu.
OpClass LLVMDynamicInst::getOpClass() const {
  auto iter = LLVMDynamicInst::instToOpClass.find(this->getInstName());
  if (iter == LLVMDynamicInst::instToOpClass.end()) {
    // For unknown, simply return IntAlu.
    return No_OpClass;
  }
  return iter->second;
}

bool LLVMDynamicInst::canWriteBack(LLVMTraceCPU *cpu) const {
  if (this->isStoreInst()) {
    for (const auto dependentInstId : this->TDG.deps()) {
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

LLVMDynamicInstMem::LLVMDynamicInstMem(const LLVM::TDG::TDGInstruction &_TDG,
                                       uint8_t _numMicroOps, Addr _align,
                                       Type _type)
    : LLVMDynamicInst(_TDG, _numMicroOps), align(_align), type(_type),
      value(nullptr) {
  if (this->type == ALLOCA) {
    if (!this->TDG.has_alloc()) {
      panic("Alloc without extra alloc information from TDG.");
    }
  } else if (this->type == LOAD) {
    panic_if(!this->TDG.has_load(), "Load without extra information from TDG.");
  } else if (this->type == STORE) {
    panic_if(!this->TDG.has_store(),
             "Store without extra information from TDG.");
    const auto &storeExtra = this->TDG.store();
    // Get the stored value.
    // Special case for memset: transform into a huge store.
    // TODO: handle this more gracefully.
    this->value = new uint8_t[storeExtra.size()];
    if (this->getInstName() == "store") {
      if (storeExtra.size() != storeExtra.value().size()) {
        panic("Unmatched stored value size, size %ul, value.size %ul.\n",
              storeExtra.size(), storeExtra.value().size());
      }
      memcpy(value, storeExtra.value().data(), storeExtra.value().size());
    } else if (this->getInstName() == "memset") {
      if (storeExtra.value().size() != 1) {
        panic("Memset with value.size %ul.\n", storeExtra.value().size());
      }
      memset(value, storeExtra.value().front(), storeExtra.size());
    }
  }
}

LLVMDynamicInstMem::~LLVMDynamicInstMem() {
  if (this->value != nullptr) {
    delete this->value;
    this->value = nullptr;
  }
}

void LLVMDynamicInstMem::execute(LLVMTraceCPU *cpu) {

  // Notify the stream engine.
  for (const auto &streamId : this->TDG.used_stream_ids()) {
    cpu->getAcceleratorManager()->useStream(streamId, this);
  }

  switch (this->type) {
  case Type::ALLOCA: {
    // We need to handle stack allocation only
    // when we have a driver.
    if (!cpu->isStandalone()) {
      if (this->TDG.alloc().new_base() == "") {
        panic("Alloc with empty new base for integrated mode.\n");
      }
      Addr vaddr = cpu->allocateStack(this->TDG.alloc().size(), this->align);
      // Set up the mapping.
      cpu->mapBaseNameToVAddr(this->TDG.alloc().new_base(), vaddr);
    }
    break;
  }
  case Type::STORE: {
    // Just construct the packets.
    // Only sent these packets at writeback.
    this->constructPackets(cpu);
    break;
  }
  case Type::LOAD: {
    this->constructPackets(cpu);
    for (const auto &packet : this->packets) {
      cpu->sendRequest(packet.paddr, packet.size, this, packet.data);
      DPRINTF(LLVMTraceCPU, "Send request paddr %p size %u for inst %d\n",
              reinterpret_cast<void *>(packet.paddr), packet.size,
              this->getId());
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
    panic("Calling writeback on non-store/load inst %u.\n", this->getId());
  }

  uint64_t size;
  if (this->type == Type::STORE) {
    size = this->TDG.store().size();
  } else {
    size = this->TDG.load().size();
  }

  for (int packetSize, inflyPacketsSize = 0; inflyPacketsSize < size;
       inflyPacketsSize += packetSize) {
    Addr paddr, vaddr;
    if (cpu->isStandalone()) {
      // When in stand alone mode, use the trace space address
      // directly as the virtual address.
      // No address translation.
      if (this->type == Type::STORE) {
        vaddr = this->TDG.store().addr() + inflyPacketsSize;
      } else {
        vaddr = this->TDG.load().addr() + inflyPacketsSize;
      }
      paddr = cpu->translateAndAllocatePhysMem(vaddr);
    } else {
      // When we have a driver, we have to translate trace space
      // address into simulation space and then use the process
      // page table to get physical address.
      if (this->type == Type::STORE) {
        vaddr = cpu->getVAddrFromBase(this->TDG.store().base()) +
                this->TDG.store().offset() + inflyPacketsSize;
      } else {
        vaddr = cpu->getVAddrFromBase(this->TDG.load().base()) +
                this->TDG.load().offset() + inflyPacketsSize;
      }
      paddr = cpu->getPAddrFromVaddr(vaddr);
      DPRINTF(LLVMTraceCPU,
              "vaddr %llu, infly %llu, paddr %llu, "
              "reintered %p\n",
              vaddr, inflyPacketsSize, paddr, reinterpret_cast<void *>(paddr));
    }
    // For now only support maximum 8 bytes access.
    packetSize = size - inflyPacketsSize;
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
            reinterpret_cast<void *>(paddr), packetSize, this->getId());
  }
}

void LLVMDynamicInstMem::writeback(LLVMTraceCPU *cpu) {
  if (this->type != Type::STORE) {
    panic("Calling writeback on non-store inst %u.\n", this->getId());
  }
  // Start to send the packets to cpu for store.
  for (const auto &packet : this->packets) {
    if (packet.size == 8) {
      DPRINTF(LLVMTraceCPU, "Store data %f for inst %u to paddr %p\n",
              *(double *)(packet.data), this->getId(), packet.paddr);
    }
    cpu->sendRequest(packet.paddr, packet.size, this, packet.data);
  }
}

bool LLVMDynamicInstMem::isCompleted() const {
  if (this->type != Type::STORE) {
    return this->packets.size() == 0 && this->remainingMicroOps == 0;
  } else {
    // For store, the infly packets doesn't control completeness.
    return this->remainingMicroOps == 0;
  }
}

bool LLVMDynamicInstMem::isWritebacked() const {
  if (this->type != Type::STORE) {
    panic("Calling isWritebacked on non-store inst %u.\n", this->getId());
  }
  return this->packets.size() == 0;
}

void LLVMDynamicInstMem::handlePacketResponse(LLVMTraceCPU *cpu,
                                              PacketPtr packet) {
  if (this->type != Type::STORE && this->type != Type::LOAD) {
    panic("LLVMDynamicInstMem::handlePacketResponse called for non store/load "
          "inst %d, but type %d.\n",
          this->getId(), this->type);
  }

  // Check if the load will produce a new base.
  if (this->type == Type::LOAD && this->TDG.load().new_base() != "") {
    uint64_t vaddr = packet->get<uint64_t>();
    cpu->mapBaseNameToVAddr(this->TDG.load().new_base(), vaddr);
  }

  // We don't care about which packet for now, just mark one completed.
  this->packets.pop_front();
  DPRINTF(LLVMTraceCPU, "Get response for inst %u, remain infly packets %d\n",
          this->getId(), this->packets.size());
}

void LLVMDynamicInstCompute::execute(LLVMTraceCPU *cpu) {
  // Notify the stream engine.
  for (const auto &streamId : this->TDG.used_stream_ids()) {
    cpu->getAcceleratorManager()->useStream(streamId, this);
  }
  this->fuLatency = cpu->getOpLatency(this->getOpClass());
}
