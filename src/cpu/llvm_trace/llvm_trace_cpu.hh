#ifndef __CPU_LLVM_TRACE_CPU_HH__
#define __CPU_LLVM_TRACE_CPU_HH__

#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "cpu/base.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_trace_cpu_driver.hh"
#include "cpu/o3/fu_pool.hh"
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
        : MasterPort(name, _owner), owner(_owner), blocked(false) {}

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
    std::mutex blockedPacketPtrsMutex;
    bool blocked;
  };

  /**
   * Read in the trace file and popluate dynamicInsts vector.
   */
  void readTraceFile();

  void tick();

  FuncPageTable pageTable;
  CPUPort instPort;
  CPUPort dataPort;

  FUPool* fuPool;

  const std::string traceFile;

  // Used to record the current stack depth, so that we can break trace
  // into multiple function calls.
  int currentStackDepth;
  LLVMDynamicInstId currentInstId;
  std::vector<std::shared_ptr<LLVMDynamicInst>> dynamicInsts;

  //*********************************************************//
  // This variables are initialized per replay.
  // A map from base name to user space address.
  std::unordered_map<std::string, Addr> mapBaseToVAddr;

  // Process and ThreadContext for the simulation program.
  Process* process;
  ThreadContext* thread_context;
  // The top of the stack for this replay.
  Addr stackMin;

  Addr finish_tag_paddr;

  enum InstStatus {
    FETCHED,  // In fetchQueue.
    READY,    // Ready to be issued.
    ISSUED,
    FINISHED,  // Finished computing.
  };
  std::unordered_map<LLVMDynamicInstId, InstStatus> inflyInsts;

  std::queue<LLVMDynamicInstId> fetchQueue;
  const size_t MAX_FETCH_QUEUE_SIZE;

  void fetch();

  // reorder buffer.
  std::vector<std::pair<LLVMDynamicInstId, bool>> rob;
  const size_t MAX_REORDER_BUFFER_SIZE;

  // Fetch instructions from fetchQueue and fill into rob.
  void reorder();

  // instruction queue.
  // Sorted.
  std::list<LLVMDynamicInstId> instructionQueue;
  const size_t MAX_INSTRUCTION_QUEUE_SIZE;

  // Find instructions ready to be issued in rob (mark them as READY).
  void markReadyInst();

  // Issue the ready instructions (mark them as ISSUED).
  void issue();

  // For issued compute insts, count down the their compute delay,
  // when reaches 0, mark them as FINISHED in inflyInsts.
  void countDownComputeDelay();

  // Commit the FINISHED inst and remove it from inflyInsts.
  void commit();

  LLVMTraceCPUDriver* driver;

  /**************************************************************/
  // Interface for the insts.
 public:
  // Check if this is running in standalone mode (no normal cpu).
  bool isStandalone() const { return this->driver == nullptr; }

  // Check if an inst is already finished.
  bool isInstFinished(LLVMDynamicInstId instId) const {
    if (instId >= this->currentInstId) {
      return false;
    }
    if (this->inflyInsts.find(instId) != this->inflyInsts.end()) {
      return false;
    }
    return true;
  }

  // Allocate the stack. Should only be used in non-standalone mode.
  // Return the virtual address.
  Addr allocateStack(Addr size, Addr align);

  /**
   * Translate the vaddr to paddr.
   * May allocate page if page fault.
   * Note: only use this in stand alone mode (no driver).
   */
  Addr translateAndAllocatePhysMem(Addr vaddr);

  // Map a base name to vaddr;
  void mapBaseNameToVAddr(const std::string& base, Addr vaddr);

  // Get a vaddr from base.
  Addr getVAddrFromBase(const std::string& base) {
    if (this->mapBaseToVAddr.find(base) == this->mapBaseToVAddr.end()) {
      panic("Failed to look up base %s\n", base.c_str());
    }
    return this->mapBaseToVAddr.at(base);
  }

  // Translate from vaddr to paddr.
  Addr getPAddrFromVaddr(Addr vaddr);

  // Send a request to memory.
  // If data is not nullptr, it will be a write.
  void sendRequest(Addr paddr, int size, LLVMDynamicInstId instId,
                   uint8_t* data);

  //********************************************************//
  // Event for this CPU.
  EventWrapper<LLVMTraceCPU, &LLVMTraceCPU::tick> tickEvent;

  // API for complete a function unit.
  // @param fuId: if not -1, will free this fu in next cycle.
  void processFUCompletion(LLVMDynamicInstId instId, int fuId);

  class FUCompletion : public Event {
   public:
    FUCompletion(LLVMDynamicInstId _instId, int _fuId, LLVMTraceCPU* _cpu,
                 bool _shouldFreeFU = false);
    // From Event.
    void process() override;
    const char* description() const override;

   private:
    LLVMDynamicInstId instId;
    int fuId;
    // Pointer back to the CPU.
    LLVMTraceCPU* cpu;
    // Should the FU be freed next cycle.
    // Used for un-pipelined FU.
    // Default is false.
    bool shouldFreeFU;
  };

 public:
  /*******************************************************************/
  // All the statics.
  /*******************************************************************/
  void regStats() override;
  // Stat for total number issued for each instruction type.
  Stats::Vector2d statIssuedInstType;
};

#endif
