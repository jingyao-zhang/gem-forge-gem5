#include "dyn_inst_stream_dispatcher.hh"

/**
 * All the other instructions.
 */
#include "accelerator/adfa/insts.hh"
#include "accelerator/speculative_precomputation/insts.hh"
#include "accelerator/stream/insts.hh"

DynamicInstructionStreamDispatcher::DynamicInstructionStreamDispatcher(
    const std::string &_fn)
    : fn(_fn) {
  this->input = new ProtoInputStream(this->fn);

  // Parse the static information.
  bool successReadStaticInfo = this->input->read(this->staticInfo);
  assert(successReadStaticInfo && "Failed to readin the static info.");

  // Create the main buffer.
  this->mainBuffer = std::make_shared<Buffer>();
  this->currentBuffer = this->mainBuffer;

  // Read in the first few instructions.
  this->parse();
}

DynamicInstructionStreamDispatcher::~DynamicInstructionStreamDispatcher() {
  delete this->input;
  this->input = nullptr;
}

void DynamicInstructionStreamDispatcher::parse() {
  if (this->currentBuffer->getSize() > 1000) {
    return;
  }

  size_t count = 0;

  while (true) {
    // Try parse the next instruction, if failed, peek_back() won't truly
    // allocate the buffer.
    auto &packet = *(this->currentBuffer->peek_back());
    if (!this->input->read(packet.tdg)) {
      // Reached the end.
      break;
    }

    // Consume this one. This one should return exactly the same as the
    // previous peek_back().
    this->currentBuffer->alloc_back();

    // Parse use-specified instruction.
    packet.inst = parseADFAInst(packet.tdg);

    if (packet.inst == nullptr) {
      packet.inst = parseStreamInst(packet.tdg);
    }

    if (packet.inst == nullptr) {
      packet.inst = parseSpeculativePrecomputationInst(packet.tdg);
    }

    if (packet.inst == nullptr) {
      // Hack for numMicroOps, which is not actually used now.
      int numMicroOps = 1;

      // Handle the extra fields.
      switch (packet.tdg.extra_case()) {
      case LLVM::TDG::TDGInstruction::ExtraCase::kStore: {
        // Store.
        packet.inst = new LLVMDynamicInstMem(packet.tdg, numMicroOps, 16,
                                             LLVMDynamicInstMem::Type::STORE);
        break;
      }

      case LLVM::TDG::TDGInstruction::ExtraCase::kLoad: {
        // Load.
        packet.inst = new LLVMDynamicInstMem(packet.tdg, numMicroOps, 16,
                                             LLVMDynamicInstMem::LOAD);
        break;
      }

      case LLVM::TDG::TDGInstruction::ExtraCase::kAlloc: {
        // Alloc.
        packet.inst = new LLVMDynamicInstMem(packet.tdg, numMicroOps, 16,
                                             LLVMDynamicInstMem::Type::ALLOCA);
        break;
      }

      case LLVM::TDG::TDGInstruction::ExtraCase::kBranch: {
        // Branch.
        packet.inst = new LLVMDynamicInstCompute(
            packet.tdg, numMicroOps, LLVMDynamicInstCompute::Type::OTHER);
        break;
      }

      case LLVM::TDG::TDGInstruction::ExtraCase::EXTRA_NOT_SET: {
        // Default instructions.
        auto type = LLVMDynamicInstCompute::Type::OTHER;

        if (packet.tdg.op() == "call") {
          type = LLVMDynamicInstCompute::Type::CALL;
        } else if (packet.tdg.op() == "ret") {
          type = LLVMDynamicInstCompute::Type::RET;
        } else if (packet.tdg.op() == "sin") {
          type = LLVMDynamicInstCompute::Type::SIN;
        } else if (packet.tdg.op() == "cos") {
          type = LLVMDynamicInstCompute::Type::COS;
        } else if (packet.tdg.op() == "cca") {
          type = LLVMDynamicInstCompute::Type::ACCELERATOR;
        }

        packet.inst = new LLVMDynamicInstCompute(packet.tdg, numMicroOps, type);
        break;
      }

      default: {
        panic("Unrecognized oneof name case.\n");
        break;
      }
      }
    }

    count++;

    // Each time we parse in at most 1000 instructions.
    if (count == 1000) {
      break;
    }
  }
}