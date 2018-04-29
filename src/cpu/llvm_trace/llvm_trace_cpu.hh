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
#include "cpu/llvm_trace/llvm_commit_stage.hh"
#include "cpu/llvm_trace/llvm_decode_stage.hh"
#include "cpu/llvm_trace/llvm_fetch_stage.hh"
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_rename_stage.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
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
  void handleReplay(Process* p, ThreadContext* tc, const std::string& trace,
                    const Addr finish_tag_vaddr,
                    std::vector<std::pair<std::string, Addr>> maps);

  enum InstStatus {
    FETCHED,     // In fetchQueue.
    DECODED,     // Decoded.
    DISPATCHED,  // Dispatched to instQueue.
    READY,       // Ready to be issued.
    ISSUED,
    FINISHED,  // Finished computing.
  };

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
      // panic("recvTimingResp not implemented.");
    }
    void sendReq(PacketPtr pkt);
    void recvReqRetry() override;

    size_t getPendingPacketsNum() const {
      return this->blockedPacketPtrs.size();
    }

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

  const std::string traceFile;

  TheISA::TLB* itb;
  TheISA::TLB* dtb;

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

  std::unordered_map<LLVMDynamicInstId, InstStatus> inflyInsts;

  friend class LLVMFetchStage;
  friend class LLVMDecodeStage;
  friend class LLVMRenameStage;
  friend class LLVMIEWStage;
  friend class LLVMCommitStage;

  LLVMFetchStage fetchStage;
  LLVMDecodeStage decodeStage;
  LLVMRenameStage renameStage;
  LLVMIEWStage iewStage;
  LLVMCommitStage commitStage;

  TimeBuffer<LLVMFetchStage::FetchStruct> fetchToDecode;
  TimeBuffer<LLVMDecodeStage::DecodeStruct> decodeToRename;
  TimeBuffer<LLVMRenameStage::RenameStruct> renameToIEW;
  TimeBuffer<LLVMIEWStage::IEWStruct> iewToCommit;
  TimeBuffer<LLVMStageSignal> signalBuffer;

  LLVMTraceCPUDriver* driver;

  /**************************************************************/
  // Interface for the insts.
 public:
  Stats::Distribution numPendingAccessDist;

  // Check if this is running in standalone mode (no normal cpu).
  bool isStandalone() const { return this->driver == nullptr; }

  // Check if an inst is already finished.
  bool isInstFinished(LLVMDynamicInstId instId) const {
    if (instId >= this->currentInstId) {
      return false;
    }
    if (this->inflyInsts.find(instId) != this->inflyInsts.end()) {
      return this->inflyInsts.at(instId) == InstStatus::FINISHED;
    }
    return true;
  }

  bool isInstCommitted(LLVMDynamicInstId instId) const {
    if (instId >= this->currentInstId) {
      return false;
    }
    // If it's out of the infly insts map, we say it committed.
    return this->inflyInsts.find(instId) == this->inflyInsts.end();
  }

  std::shared_ptr<LLVMDynamicInst> getDynamicInst(LLVMDynamicInstId instId) {
    return this->dynamicInsts[instId];
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

 public:
  /*******************************************************************/
  // All the statics.
  /*******************************************************************/
  void regStats() override;
};

#endif
