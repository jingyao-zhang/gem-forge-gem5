#ifndef __CPU_LLVM_TRACE_CPU_HH__
#define __CPU_LLVM_TRACE_CPU_HH__

#include <queue>
#include <unordered_set>
#include <vector>

#include "cpu/base.hh"
#include "cpu/llvm_trace/llvm_trace_cpu_driver.hh"
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

  // API interface for driver.
  void handleMapVirtualMem(Process* p, ThreadContext* tc,
                           const std::string& base, const Addr vaddr);
  void handleReplay(Process* p, ThreadContext* tc, const std::string& trace,
                    const Addr finish_tag_vaddr);

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
    void recvReqRetry() override;

   private:
    LLVMTraceCPU* owner;
    // Blocked packets for flow control.
    // Note: for now I don't handle thread safety as there
    // would be only one infly instructions. Is gem5 single
    // thread?
    std::queue<PacketPtr> blockedPacketPtrs;
  };

  using DynamicInstId = uint64_t;
  struct DynamicInst {
    enum Type : uint8_t {
      LOAD,
      STORE,
      COMPUTE,
    } type;
    Tick computeDelay;
    // Used for LOAD/STORE.
    int size;
    std::string base;
    Addr offset;
    Addr trace_vaddr;  // Trace space vaddr.
    // Used for STORE.
    void* value;
    DynamicInst(Type _type, Tick _computeDelay, int _size,
                const std::string& _base, Addr _offset = 0x0,
                Addr _trace_vaddr = 0x0, void* _value = nullptr)
        : type(_type),
          computeDelay(_computeDelay),
          size(_size),
          base(_base),
          offset(_offset),
          trace_vaddr(_trace_vaddr),
          value(_value) {}
  };

  /**
   * Read in the trace file and popluate dynamicInsts vector.
   */
  void readTraceFile();

  void processScheduleNextEvent();

  /**
   * Translate the vaddr to paddr.
   * May allocate page if page fault.
   * Note: only use this in stand alone mode (no driver).
   */
  Addr translateAndAllocatePhysMem(Addr vaddr);

  /**
   * Translate the trace vaddr to simulation vaddr
   * then to simulation paddr.
   * In this way we do not model TLB timing information.
   * TODO: Look into gem5's TLB model and integrate with it.
   * Note: only use this with driver (so that base is initialized).
   */
  Addr translateTraceToPhysMem(const std::string& base, Addr offset);

  FuncPageTable pageTable;
  CPUPort instPort;
  CPUPort dataPort;

  const std::string traceFile;

  DynamicInstId currentInstId;
  std::vector<DynamicInst> dynamicInsts;
  std::unordered_set<DynamicInstId> inflyInstIds;

  //*********************************************************//
  // A map from base name to user space address.
  std::unordered_map<std::string, Addr> mapBaseToVAddr;

  // Process and ThreadContext for the simulation program.
  Process* process;
  ThreadContext* thread_context;

  Addr finish_tag_paddr;

  //********************************************************//
  EventWrapper<LLVMTraceCPU, &LLVMTraceCPU::processScheduleNextEvent>
      scheduleNextEvent;

  LLVMTraceCPUDriver* driver;
};

#endif
