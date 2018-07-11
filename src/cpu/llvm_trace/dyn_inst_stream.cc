#include "dyn_inst_stream.hh"

// For the DPRINTF function.
#include "base/trace.hh"
#include "debug/LLVMTraceCPU.hh"

DynamicInstructionStream::DynamicInstructionStream(const std::string &_fn)
    : fn(_fn) {
  this->input = new ProtoInputStream(this->fn);
  this->_fetchSize = 0;
}

size_t DynamicInstructionStream::parse() {
  size_t count = 0;

  auto &inst = *(this->buffer.alloc_back());

  while (this->input->read(inst)) {
    LLVMDynamicInst *llvmInst;

    // Hack for numMicroOps, which is not actually used now.
    int numMicroOps = 1;

    std::vector<LLVMDynamicInstId> dependentInstIds;
    for (size_t depI = 0; depI < inst.deps_size(); ++depI) {
      dependentInstIds.push_back(inst.deps(depI));
    }

    // Handle the extra fields.
    switch (inst.extra_case()) {
    case LLVM::TDG::TDGInstruction::ExtraCase::kStore: {
      // Store.
      const auto &storeExtra = inst.store();
      // Get the stored value.
      // Special case for memset: transform into a huge store.
      // TODO: handle this more gracefully.
      uint8_t *value = new uint8_t[storeExtra.size()];
      if (inst.op() == "store") {
        if (storeExtra.size() != storeExtra.value().size()) {
          panic("Unmatched stored value size, size %ul, value.size %ul.\n",
                storeExtra.size(), storeExtra.value().size());
        }
        memcpy(value, storeExtra.value().data(), storeExtra.value().size());
      } else if (inst.op() == "memset") {
        if (storeExtra.value().size() != 1) {
          panic("Memset with value.size %ul.\n", storeExtra.value().size());
        }
        memset(value, storeExtra.value().front(), storeExtra.size());
      }

      llvmInst = new LLVMDynamicInstMem(
          inst.id(), inst.op(), numMicroOps, std::move(dependentInstIds),
          storeExtra.size(), storeExtra.base(), storeExtra.offset(),
          storeExtra.addr(), 16, LLVMDynamicInstMem::Type::STORE, value,
          "" /* new_base */
      );
      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::kLoad: {
      // Load.
      const auto &loadExtra = inst.load();
      llvmInst = new LLVMDynamicInstMem(
          inst.id(), inst.op(), numMicroOps, std::move(dependentInstIds),
          loadExtra.size(), loadExtra.base(), loadExtra.offset(),
          loadExtra.addr(), 16, LLVMDynamicInstMem::LOAD,
          /* value */ nullptr, loadExtra.new_base());
      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::kAlloc: {
      // Alloc.
      const auto &allocExtra = inst.alloc();
      llvmInst = new LLVMDynamicInstMem(
          inst.id(), inst.op(), numMicroOps, std::move(dependentInstIds),
          allocExtra.size(), "" /*base*/, 0 /*offset*/, allocExtra.addr(), 16,
          LLVMDynamicInstMem::Type::ALLOCA, nullptr /*value*/,
          allocExtra.new_base());
      if (allocExtra.new_base() == "") {
        panic("Alloc without valid new base.\n");
      }
      DPRINTF(LLVMTraceCPU, "Alloc with new base %s\n",
              allocExtra.new_base().c_str());

      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::kBranch: {
      // Branch.
      const auto &branchExtra = inst.branch();

      llvmInst = new LLVMDynamicInstCompute(
          inst.id(), inst.op(), numMicroOps, std::move(dependentInstIds),
          LLVMDynamicInstCompute::Type::OTHER, nullptr /*context*/);

      llvmInst->setStaticInstAddress(branchExtra.static_id());
      llvmInst->setNextBBName(branchExtra.next_bb());

      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::EXTRA_NOT_SET: {
      // Default instructions.
      auto type = LLVMDynamicInstCompute::Type::OTHER;

      LLVMAcceleratorContext *context = nullptr;
      if (inst.op() == "call") {
        type = LLVMDynamicInstCompute::Type::CALL;
      } else if (inst.op() == "ret") {
        type = LLVMDynamicInstCompute::Type::RET;
      } else if (inst.op() == "sin") {
        type = LLVMDynamicInstCompute::Type::SIN;
      } else if (inst.op() == "cos") {
        type = LLVMDynamicInstCompute::Type::COS;
      } else if (inst.op() == "cca") {
        type = LLVMDynamicInstCompute::Type::ACCELERATOR;
        // For now just use one cycle latency for cca.
        context = LLVMAcceleratorContext::parseContext("1");
      }

      llvmInst = new LLVMDynamicInstCompute(inst.id(), inst.op(), numMicroOps,
                                            std::move(dependentInstIds), type,
                                            context);

      break;
    }
    default: {
      panic("Unrecognized oneof name case.\n");
      break;
    }
    }

    this->insts.push_back(llvmInst);
    if (this->fetchSize() == 0) {
      // First time, intialize the fetch pos.
      this->fetchPos = this->insts.end();
      this->fetchPos--;
    }
    this->_fetchSize++;
    count++;

    // Each time we parse in at most 1000 instructions.
    if (count == 1000) {
      break;
    }
  }

  this->buffer.release_front();

  return count;
}

LLVMDynamicInst *DynamicInstructionStream::fetch() {
  if (this->fetchEmpty()) {
    panic("Fetch from empty instruction stream.");
  }
  auto fetched = *this->fetchPos;
  this->fetchPos++;
  this->_fetchSize--;
  return fetched;
}

void DynamicInstructionStream::commit(LLVMDynamicInst *inst) {
  if (this->empty()) {
    panic("Commit called for empty instruction stream.");
  }
  if (this->insts.front() != inst) {
    panic("Commit called out of order.");
  }
  this->insts.pop_front();
  delete inst;
}