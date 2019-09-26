#include "minor_cpu_delegator.hh"

#include "debug/MinorCPUDelegator.hh"

#if THE_ISA == RISCV_ISA
#include "cpu/gem_forge/accelerator/arch/riscv/gem_forge_isa_handler.hh"
#else
#error "Unsupported ISA"
#endif

class MinorCPUDelegator::Impl {
public:
  Impl(MinorCPU *_cpu, MinorCPUDelegator *_cpuDelegator)
      : cpu(_cpu), cpuDelegator(_cpuDelegator) {}

  MinorCPU *cpu;
  MinorCPUDelegator *cpuDelegator;

  // Cache of the traceExtraFolder.
  std::string traceExtraFolder;

  Process *getProcess() {
    assert(this->cpu->threads.size() == 1 &&
           "SMT not supported in GemForge yet.");
    // Crazy oracle access chain.
    auto thread = this->cpu->threads.front();
    auto process = thread->getProcessPtr();
    return process;
  }

  uint64_t getInstSeqNum(Minor::MinorDynInstPtr dynInstPtr) const {
    auto seqNum = dynInstPtr->id.execSeqNum;
    assert(seqNum != 0 && "GemForge assumes SeqNum 0 is reserved as invalid.");
    return seqNum;
  }
};

/**********************************************************************
 * MinorCPUDelegator.
 *********************************************************************/

MinorCPUDelegator::MinorCPUDelegator(MinorCPU *_cpu)
    : GemForgeCPUDelegator(CPUTypeE::MINOR, _cpu), pimpl(new Impl(_cpu, this)) {
}
MinorCPUDelegator::~MinorCPUDelegator() = default;

bool MinorCPUDelegator::canDispatch(Minor::MinorDynInstPtr dynInstPtr) {
  assert(dynInstPtr->isInst() && "Should be a real inst.");
  return true;
}

const std::string &MinorCPUDelegator::getTraceExtraFolder() const {
  // Always assume that the binary is in the TraceExtraFolder.
  if (pimpl->traceExtraFolder.empty()) {
    auto process = pimpl->getProcess();
    const auto &executable = process->executable;
    auto sepPos = executable.rfind('/');
    if (sepPos == std::string::npos) {
      // Not found.
      pimpl->traceExtraFolder = ".";
    } else {
      pimpl->traceExtraFolder = executable.substr(0, sepPos);
    }
  }
  return pimpl->traceExtraFolder;
}

Addr MinorCPUDelegator::translateVAddrOracle(Addr vaddr) {
  auto process = pimpl->getProcess();
  auto pTable = process->pTable;
  Addr paddr;
  if (pTable->translate(vaddr, paddr)) {
    return paddr;
  }
  // TODO: Let the caller handle this.
  panic("Translate vaddr failed %#x.", vaddr);
  return paddr;
}

void MinorCPUDelegator::sendRequest(PacketPtr pkt) {
  panic("MinorCPUDelegator::sendRequest not yet supported.");
}
