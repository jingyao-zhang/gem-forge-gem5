#include "dyn_inst_stream.hh"

// For the DPRINTF function.
#include "base/trace.hh"
#include "debug/LLVMTraceCPU.hh"

DynamicInstructionStream::DynamicInstructionStream(const std::string &_fn)
    : fn(_fn) {
  this->input = new ProtoInputStream(this->fn);
  this->_fetchSize = 0;
}

DynamicInstructionStream::~DynamicInstructionStream() {
  delete this->input;
  this->input = nullptr;
}

size_t DynamicInstructionStream::parse() {
  size_t count = 0;

  while (true) {

    // Try parse the next instruction, if failed, peek_back() won't truly
    // allocate the buffer.
    auto &inst = *(this->buffer.peek_back());
    if (!this->input->read(inst)) {
      // Reached the end.
      break;
    }

    // Consume this one. This one should return exactly the same as the
    // previous peek_back().
    this->buffer.alloc_back();

    LLVMDynamicInst *llvmInst;

    // Hack for numMicroOps, which is not actually used now.
    int numMicroOps = 1;

    // Handle the extra fields.
    switch (inst.extra_case()) {
    case LLVM::TDG::TDGInstruction::ExtraCase::kStore: {
      // Store.
      llvmInst = new LLVMDynamicInstMem(inst, numMicroOps, 16,
                                        LLVMDynamicInstMem::Type::STORE);
      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::kLoad: {
      // Load.
      llvmInst = new LLVMDynamicInstMem(inst, numMicroOps, 16,
                                        LLVMDynamicInstMem::LOAD);
      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::kAlloc: {
      // Alloc.
      llvmInst = new LLVMDynamicInstMem(inst, numMicroOps, 16,
                                        LLVMDynamicInstMem::Type::ALLOCA);
      break;
    }

    case LLVM::TDG::TDGInstruction::ExtraCase::kBranch: {
      // Branch.
      llvmInst = new LLVMDynamicInstCompute(inst, numMicroOps,
                                            LLVMDynamicInstCompute::Type::OTHER,
                                            nullptr /*context*/);
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

      llvmInst = new LLVMDynamicInstCompute(inst, numMicroOps, type, context);

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
  this->buffer.release_front(&inst->getTDG());
  delete inst;
}