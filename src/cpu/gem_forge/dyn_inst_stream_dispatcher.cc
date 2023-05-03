#include "dyn_inst_stream_dispatcher.hh"

/**
 * All the other instructions.
 */
#include "accelerator/adfa/insts.hh"
#include "accelerator/speculative_precomputation/insts.hh"
#include "accelerator/stream/insts.hh"

namespace gem5 {

DynamicInstructionStreamDispatcher::DynamicInstructionStreamDispatcher(
    const std::string &_fn, bool _enableADFA)
    : fn(_fn), enableADFA(_enableADFA), input(nullptr), regionTable(nullptr),
      inContinuousRegion(false) {
  this->input = new ProtoInputStream(this->fn);

  // Parse the static information.
  panic_if(!this->input->read(this->staticInfo),
           "Failed to readin the static info.");

  // Create the region table.
  this->regionTable = new RegionTable(this->staticInfo);

  // Create the main buffer.
  this->mainBuffer = std::make_shared<Buffer>();
  this->currentBuffer = this->mainBuffer;

  // Read in the first few instructions.
  this->parse();
}

DynamicInstructionStreamDispatcher::~DynamicInstructionStreamDispatcher() {
  delete this->input;
  this->input = nullptr;

  delete this->regionTable;
  this->regionTable = nullptr;
}

void DynamicInstructionStreamDispatcher::parse() {
  size_t count = 0;

  while (true) {
    if (this->currentBuffer->getSize() > 1000) {
      break;
    }

    // Try parse the next instruction, if failed, peek_back() won't truly
    // allocate the buffer.
    auto &packet = *(this->currentBuffer->peek_back());
    // ! Remember to clear the fields.
    packet.released = false;
    packet.inst = nullptr;
    if (!this->input->read(packet.tdg)) {
      // Reached the end.
      break;
    }

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

    // Dispatch the instruction.
    this->dispatch(&packet);

    count++;

    // Each time we parse in at most 1000 instructions.
    if (count == 1000) {
      break;
    }
  }
}

void DynamicInstructionStreamDispatcher::dispatch(Packet *packet) {
  if (this->enableADFA) {
    this->dispatchADFA(packet);
    return;
  }

  // Normally push into the current buffer.
  this->currentBuffer->alloc_back();
}

void DynamicInstructionStreamDispatcher::dispatchADFA(Packet *packet) {
  // inform("Dispatch %s.\n", packet->tdg.op().c_str());
  if (packet->tdg.bb() != 0) {
    if (this->regionTable->hasRegionSetFromBB(packet->tdg.bb())) {
      bool newRegionContinuous = false;
      for (const auto &regionId :
           this->regionTable->getRegionSetFromBB(packet->tdg.bb())) {
        const auto &region = this->regionTable->getRegionFromRegionId(regionId);
        if (region.continuous()) {
          newRegionContinuous = true;
          break;
        }
      }
      if (newRegionContinuous && !this->inContinuousRegion) {
        // Switch into the continuous region. Dispatch to a new buffer.
        this->inContinuousRegion = true;

        // Steal the packet from the current buffer.
        this->currentBuffer->steal_back();

        // Create a new buffer for the ADFA instruction stream.
        auto ADFABuffer = std::make_shared<Buffer>();

        // Allocate a special ADFAStart instruction in the current buffer.
        // ! Although this will break the sequence number a little bit, but
        // ! as we only do this when enter/leaving a region, so it should be
        // fine.
        auto ADFAStartPacket = this->currentBuffer->alloc_back();
        ADFAStartPacket->released = false;
        // ! Remember to clear the deps.
        ADFAStartPacket->tdg.clear_deps();
        ADFAStartPacket->tdg.set_op("df-start");
        ADFAStartPacket->tdg.set_id(LLVMDynamicInst::allocateDynamicInstId());

        ADFAStartPacket->inst =
            new ADFAStartInst(ADFAStartPacket->tdg, ADFABuffer);

        // Push the new packet into the ADFABuffer.
        ADFABuffer->push_back(packet);

        // Switch to the ADFABuffer.
        this->currentBuffer = ADFABuffer;

        return;
      } else if (!newRegionContinuous && this->inContinuousRegion) {
        // Switch out of the continuous region.
        this->inContinuousRegion = false;

        // Steal the packet from the current (ADFA) buffer.
        this->currentBuffer->steal_back();

        // Allocate a ADFAEnd token in the current buffer.
        auto ADFAEndPacket = this->currentBuffer->alloc_back();
        ADFAEndPacket->released = false;
        // ! Remember to clear the deps.
        ADFAEndPacket->tdg.clear_deps();
        ADFAEndPacket->tdg.set_op("df-end");
        ADFAEndPacket->tdg.set_id(LLVMDynamicInst::allocateDynamicInstId());
        ADFAEndPacket->inst =
            new LLVMDynamicInstCompute(ADFAEndPacket->tdg, 1 /*numMicroOps*/,
                                       LLVMDynamicInstCompute::Type::OTHER);

        // Push the new packet into the MainBuffer.
        this->mainBuffer->push_back(packet);

        // Switch back to main buffer.
        this->currentBuffer = this->mainBuffer;

        return;
      }
    }
  }
  // Otherwise, normally push into the current buffer.
  this->currentBuffer->alloc_back();
}
} // namespace gem5
