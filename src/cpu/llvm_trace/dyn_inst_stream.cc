#include "dyn_inst_stream.hh"

// For the DPRINTF function.
#include "base/trace.hh"
#include "debug/LLVMTraceCPU.hh"

/**
 * All the other instructions.
 */
#include "accelerator/adfa/insts.hh"
#include "accelerator/stream/insts.hh"

DynamicInstructionStream::DynamicInstructionStream(const std::string &_fn)
    : fn(_fn) {
  this->input = new ProtoInputStream(this->fn);
  this->_fetchSize = 0;

  // Parse the static information.
  this->input->read(this->staticInfo);

  // Read in the first few instructions.
  this->parse();
}

DynamicInstructionStream::~DynamicInstructionStream() {
  delete this->input;
  this->input = nullptr;
}

const LLVM::TDG::StaticInformation &
DynamicInstructionStream::getStaticInfo() const {
  return this->staticInfo;
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

    LLVMDynamicInst *llvmInst = nullptr;

    // Parse use-specified instruction.
    llvmInst = parseADFAInst(inst);

    if (llvmInst == nullptr) {
      llvmInst = parseStreamInst(inst);
    }

    if (llvmInst == nullptr) {
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
        llvmInst = new LLVMDynamicInstCompute(
            inst, numMicroOps, LLVMDynamicInstCompute::Type::OTHER);
        break;
      }

      case LLVM::TDG::TDGInstruction::ExtraCase::EXTRA_NOT_SET: {
        // Default instructions.
        auto type = LLVMDynamicInstCompute::Type::OTHER;

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
        }

        llvmInst = new LLVMDynamicInstCompute(inst, numMicroOps, type);
        break;
      }

      default: {
        panic("Unrecognized oneof name case.\n");
        break;
      }
      }
    }

    this->insts.emplace_back(llvmInst, false);
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
  auto fetched = this->fetchPos->first;
  this->fetchPos++;
  this->_fetchSize--;
  if (this->fetchSize() < 1000) {
    this->parse();
  }
  return fetched;
}

DynamicInstructionStream::Iterator DynamicInstructionStream::fetchIter() {
  if (this->fetchEmpty()) {
    panic("Fetch from empty instruction stream.");
  }
  auto fetched = this->fetchPos;
  this->fetchPos++;
  this->_fetchSize--;
  if (this->fetchSize() < 1000) {
    this->parse();
  }
  return fetched;
}

void DynamicInstructionStream::commit(LLVMDynamicInst *inst) {
  if (this->empty()) {
    panic("Commit called for empty instruction stream.");
  }
  if (this->insts.front().first != inst) {
    panic("Commit called out of order.");
  }
  if (this->insts.front().second) {
    panic("Double commit an instruction.");
  }
  this->insts.front().second = true;
  this->release();
}

void DynamicInstructionStream::commit(Iterator instIter) {
  if (this->empty()) {
    panic("Commit called for empty instruction stream.");
  }
  instIter->second = true;
  this->release();
}

void DynamicInstructionStream::release() {
  while (!this->empty() && this->insts.front().second) {
    auto inst = this->insts.front().first;
    this->insts.pop_front();
    this->buffer.release_front(&inst->getTDG());
    delete inst;
  }
}

DynamicInstructionStreamInterfaceConditionalEnd::
    DynamicInstructionStreamInterfaceConditionalEnd(
        DynamicInstructionStream *_stream, EndFunc _endFunc)
    : stream(_stream), endFunc(_endFunc), endToken(nullptr), fetchedSize(0),
      ended(false) {}

DynamicInstructionStreamInterfaceConditionalEnd::
    ~DynamicInstructionStreamInterfaceConditionalEnd() {
  // Remember to commit the endToken if there is one.
  if (this->endToken != nullptr) {
    this->commit(this->endToken);
    this->endToken = nullptr;
  }
  if (this->fetchedSize != 0) {
    panic("Not all fetched instructions are committed before releasing this "
          "conditional end stream interface.");
  }
}

LLVMDynamicInst *DynamicInstructionStreamInterfaceConditionalEnd::fetch() {
  if (this->ended) {
    return nullptr;
  }
  if (this->stream->fetchEmpty()) {
    // We have reached the end of the underneath stream.
    this->ended = true;
    return nullptr;
  }
  auto Iter = this->stream->fetchIter();
  if (this->fetchedSize == 0) {
    this->headIter = Iter;
  }
  this->fetchedSize++;
  // Check for endToken.
  if (this->endFunc(Iter->first)) {
    if (this->endToken != nullptr) {
      panic("We have already fetched the end token.");
    }
    this->endToken = Iter->first;
    this->ended = true;
    // We have reached the end.
    return nullptr;
  }
  return Iter->first;
}

void DynamicInstructionStreamInterfaceConditionalEnd::commit(
    LLVMDynamicInst *inst) {
  if (this->fetchedSize == 0) {
    panic("Try to commit when we have fetched nothing.");
  }
  if (this->headIter->first != inst) {
    panic("Instruction is not committed in order.");
  }
  auto committedIter = this->headIter;
  ++this->headIter;
  this->stream->commit(committedIter);
  --this->fetchedSize;
}
