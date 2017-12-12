#ifndef __CPU_LLVM_TRACE_CPU_HH__
#define __CPU_LLVM_TRACE_CPU_HH__

#include <unordered_set>
#include <vector>

#include "cpu/base.hh"
#include "mem/page_table.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU : public BaseCPU {
 public:
  LLVMTraceCPU(LLVMTraceCPUParams* params);
  ~LLVMTraceCPU();

  MasterPort& getDataPort() override { return this->dataPort; }

  MasterPort& getInstPort() override { return this->instPort; }

  Counter totalInsts() const override { return 0; }

  Counter totalOps() const override { return 0; }

  void wakeup(ThreadID tid) override {}

  bool handleTimingResp(PacketPtr pkt);

 private:
  // This port will handle retry.
  class CPUPort : public MasterPort {
   public:
    CPUPort(const std::string& name, LLVMTraceCPU* _owner)
        : MasterPort(name, _owner), owner(_owner) {}

    bool recvTimingResp(PacketPtr pkt) override {
      return this->owner->handleTimingResp(pkt);
    }
    void recvTimingSnoopReq(PacketPtr pkt) override {
      panic("recvTimingResp not implemented.");
    }
    void sendReq(PacketPtr pkt);
    void recvReqRetry() override { panic("recvReqRetry not implemented."); }

   private:
    LLVMTraceCPU* owner;
  };

  using DynamicInstId = uint64_t;
  struct DynamicInst {
    enum Type : uint8_t {
      LOAD,
      STORE,
      COMPUTE,
    } type;
    Tick computeDelay;
    // Unused if this is not LOAD/STORE.
    Addr vaddr;
    DynamicInst(Type _type, Tick _computeDelay, Addr _vaddr = 0x0)
        : type(_type), computeDelay(_computeDelay), vaddr(_vaddr) {}
  };

  /**
   * Read in the trace file and popluate dynamicInsts vector.
   */
  void readTraceFile();

  void processScheduleNextEvent();

  /**
   * Translate the vaddr to paddr.
   * May allocate page if page fault.
   */
  Addr translateAndAllocatePhysMem(Addr vaddr);

  FuncPageTable pageTable;
  CPUPort instPort;
  CPUPort dataPort;

  const std::string traceFile;

  DynamicInstId currentInstId;
  std::vector<DynamicInst> dynamicInsts;
  std::unordered_set<DynamicInstId> inflyInstIds;

  EventWrapper<LLVMTraceCPU, &LLVMTraceCPU::processScheduleNextEvent>
      scheduleNextEvent;
};

#endif
