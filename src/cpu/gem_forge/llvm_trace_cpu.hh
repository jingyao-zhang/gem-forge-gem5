#ifndef __CPU_LLVM_TRACE_CPU_HH__
#define __CPU_LLVM_TRACE_CPU_HH__

#include <fstream>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gem_forge_packet_handler.hh"

#include "base/statistics.hh"
#include "cpu/base.hh"
#include "cpu/gem_forge/llvm_commit_stage.hh"
#include "cpu/gem_forge/llvm_decode_stage.hh"
#include "cpu/gem_forge/llvm_fetch_stage.hh"
#include "cpu/gem_forge/llvm_iew_stage.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "cpu/gem_forge/llvm_rename_stage.hh"
#include "cpu/gem_forge/llvm_stage_signal.hh"
#include "cpu/gem_forge/llvm_trace_cpu_driver.hh"
#include "cpu/gem_forge/profiler/run_time_profiler.hh"
#include "cpu/gem_forge/thread_context.hh"
#include "cpu/o3/fu_pool.hh"
#include "mem/page_table.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPUDelegator;

class LLVMTraceCPU : public BaseCPU {
public:
  LLVMTraceCPU(LLVMTraceCPUParams *params);
  ~LLVMTraceCPU();

  MasterPort &getDataPort() override { return this->dataPort; }

  MasterPort &getInstPort() override { return this->instPort; }

  Counter totalInsts() const override { return 0; }

  Counter totalOps() const override { return 0; }

  void wakeup(ThreadID tid) override {}

  void init() override;

  bool handleTimingResp(PacketPtr pkt);

  // API interface for driver.
  void handleReplay(Process *p, ThreadContext *tc, const std::string &trace,
                    const Addr finish_tag_vaddr,
                    std::vector<std::pair<std::string, Addr>> maps);

  const LLVMTraceCPUParams *getLLVMTraceCPUParams() const {
    return dynamic_cast<const LLVMTraceCPUParams *>(this->params());
  }

  enum InstStatus {
    FETCHED,      // In FetchQueue.
    DECODED,      // Decoded.
    DISPATCHED,   // Dispatched to instQueue.
    BLOCKED,      // Blocked by memory, should not check ready unless unblock.
    READY,        // Ready to be issued.
    ISSUED,       // Issue to FU.
    FINISHED,     // Finished computing.
    COMMIT,       // Sent to commit stage.
    COMMITTING,   // Committing.
    COMMITTED,    // Committed.
    WRITEBACKING, // Writing back.
    WRITEBACKED,  // Write backed.
  };
  InstStatus getInflyInstStatus(LLVMDynamicInstId instId) const {
    return this->inflyInstStatus.at(instId);
  }
  void updateInflyInstStatus(LLVMDynamicInstId instId, InstStatus newStatus) {
    auto &status = this->inflyInstStatus.at(instId);
    if (status > newStatus) {
      panic("InstStatus should always be increasing.");
    }
    status = newStatus;
  }
  LLVMTraceThreadContext *getInflyInstThread(LLVMDynamicInstId instId) const {
    return this->inflyInstThread.at(instId);
  }
  LLVMTraceThreadContext *getMainThread() { return this->mainThread; }

private:
  // This port will handle retry.
  class CPUPort : public MasterPort {
  public:
    CPUPort(const std::string &name, LLVMTraceCPU *_owner)
        : MasterPort(name, _owner), owner(_owner), inflyNumPackets(0),
          blocked(false) {}

    bool recvTimingResp(PacketPtr pkt) override;
    void recvTimingSnoopReq(PacketPtr pkt) override {
      // panic("recvTimingResp not implemented.");
    }
    // void regStats();
    void sendReq();
    void addReq(PacketPtr pkt);
    void recvReqRetry() override;

    size_t getPendingPacketsNum() const {
      return this->blockedPacketPtrs.size();
    }

    bool isBlocked() const;

  private:
    LLVMTraceCPU *owner;
    // Blocked packets for flow control.
    // Note: for now I don't handle thread safety as there
    // would be only one infly instructions. Is gem5 single
    // thread?
    std::queue<PacketPtr> blockedPacketPtrs;
    std::mutex blockedPacketPtrsMutex;
    int inflyNumPackets;
    bool blocked;

    // Stats::Distribution numIssuedPackets;
  };

  void tick();

public:
  const LLVMTraceCPUParams *cpuParams;
  EmulationPageTable pageTable;
  CPUPort instPort;
  CPUPort dataPort;

  const std::string &getTraceFileName() const { return this->traceFileName; }
  const std::string &getTraceFolder() const { return this->traceFolder; }
  const std::string &getTraceExtraFolder() const {
    return this->traceExtraFolder;
  }
  LLVMTraceCPUDelegator *getDelegator() { return this->cpuDelegator.get(); }
  LLVMIEWStage &getIEWStage() { return this->iewStage; }

private:
  const std::string traceFileName;

  const unsigned totalActiveCPUs;

  std::string traceFolder;
  std::string traceExtraFolder;

  enum CPUStatusE {
    INITIALIZED,
    CACHE_WARMING,
    CACHE_WARMED,
    EXECUTING,
    DONE,
  };

  CPUStatusE cpuStatus;

  /**
   * This is the current main thread executed.
   */
  LLVMTraceThreadContext *mainThread = nullptr;
  /**
   * Map from hardware context to active threads.
   * nullptr means no active thread mapped to that context.
   */
  std::vector<LLVMTraceThreadContext *> activeThreads;

  FUPool *fuPool;

  /**
   * Should be part of process instead.
   */
  RunTimeProfiler *runTimeProfiler;

  // Used to record the current stack depth, so that we can break trace
  // into multiple function calls.
  int currentStackDepth;
  std::list<Addr> framePointerStack;
  void stackPop();
  void stackPush();

  // Setup the initial memory snapshot via functional store.
  void initializeMemorySnapshot();
  bool initializeMemorySnapshotDone;

  // Cache warm up in standalone mode.
  struct CacheWarmer : public GemForgePacketHandler {
    LLVMTraceCPU *cpu;
    const size_t repeatedTimes;
    size_t warmUpAddrs;
    size_t receivedPackets;
    LLVM::TDG::CacheWarmUp cacheWarmUpProto;
    CacheWarmer(LLVMTraceCPU *_cpu, const std::string &fn,
                size_t _repeatedTimes);
    bool isDone() const {
      return this->warmUpAddrs ==
                 this->cacheWarmUpProto.requests_size() * this->repeatedTimes &&
             this->receivedPackets == this->warmUpAddrs;
    }
    bool isDoneWithPreviousRequest() const {
      return this->receivedPackets == this->warmUpAddrs;
    }
    PacketPtr getNextWarmUpPacket();
    void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                              PacketPtr packet) override;
    void issueToMemoryCallback(GemForgeCPUDelegator *cpuDelegator) override {}
  };
  bool warmUpDone;
  CacheWarmer *cacheWarmer;

  // In fly instructions.
  std::unordered_map<LLVMDynamicInstId, LLVMDynamicInst *> inflyInstMap;

  // The status of infly instructions.
  std::unordered_map<LLVMDynamicInstId, InstStatus> inflyInstStatus;

  // Which thread does this instruction belongs to.
  std::unordered_map<LLVMDynamicInstId, LLVMTraceThreadContext *>
      inflyInstThread;

  //*********************************************************//
  // This variables are initialized per replay.
  // A map from base name to user space address.
  std::unordered_map<std::string, Addr> mapBaseToVAddr;

  // Process and ThreadContext for the simulation program.
  Process *process;
  ThreadContext *thread_context;
  ::Loader::SymbolTable symbol_table;
  // The top of the stack for this replay.
  Addr stackMin;

  Addr finish_tag_paddr;

  /**
   * Delegator for GemForgeAccelerators.
   */
  friend class LLVMTraceCPUDelegator;
  std::unique_ptr<LLVMTraceCPUDelegator> cpuDelegator;

  friend class LLVMFetchStage;
  friend class LLVMDecodeStage;
  friend class LLVMRenameStage;
  friend class LLVMIEWStage;
  friend class LLVMCommitStage;
  friend class GemForgeLoadStoreQueue;

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

  LLVMTraceCPUDriver *driver;

  /**************************************************************/
  // Interface for the insts.
public:
  Stats::Distribution numPendingAccessDist;
  Stats::Distribution numOutstandingAccessDist;

  // Check if this is running in standalone mode (no normal cpu).
  bool isStandalone() const { return this->driver == nullptr; }

  // Check if an inst is already finished.
  // We assume the instruction stream is always a feasible one.
  // So the dependent inst is guaranteed to be fetched.
  // So if it is missing from the infly status map, we assume it
  // has already been committed.
  bool isInstFinished(LLVMDynamicInstId instId) const {
    auto iter = this->inflyInstStatus.find(instId);
    if (iter != this->inflyInstStatus.end()) {
      return iter->second >= InstStatus::FINISHED;
    }
    return true;
  }

  bool isInstCommitted(LLVMDynamicInstId instId) const {
    // If it's out of the infly insts map, we say it committed.
    return this->inflyInstStatus.find(instId) == this->inflyInstStatus.end();
  }

  LLVMDynamicInst *getInflyInst(LLVMDynamicInstId id);
  LLVMDynamicInst *getInflyInstNullable(LLVMDynamicInstId id);

  // Allocate the stack. Should only be used in non-standalone mode.
  // Return the virtual address.
  Addr allocateStack(Addr size, Addr align);

  // Allocate a new thread Id.
  static ContextID allocateContextID();

  // Add the thread to our threads.
  void activateThread(LLVMTraceThreadContext *thread);
  void deactivateThread(LLVMTraceThreadContext *thread);
  size_t getNumActiveThreads() const;
  size_t getNumActiveNonIdealThreads() const;
  size_t getNumContexts() const { return this->activeThreads.size(); }

  /**
   * Translate the vaddr to paddr.
   * May allocate page if page fault.
   * Note: only use this in stand alone mode (no driver).
   */
  Addr translateAndAllocatePhysMem(Addr vaddr);

  // Map a base name to vaddr;
  void mapBaseNameToVAddr(const std::string &base, Addr vaddr);

  // Get a vaddr from base.
  Addr getVAddrFromBase(const std::string &base);

  // Translate from vaddr to paddr.
  Addr getPAddrFromVaddr(Addr vaddr);

  // Send a request to memory.
  void sendRequest(PacketPtr pkt);

  Cycles getOpLatency(OpClass opClass);

  GemForgeAcceleratorManager *getAcceleratorManager() {
    assert(this->accelManager && "Failed to get AcceleratorManager.");
    return this->accelManager;
  }

  RunTimeProfiler *getRunTimeProfiler() { return this->runTimeProfiler; }

  //********************************************************//
  // Event for this CPU.
  EventWrapper<LLVMTraceCPU, &LLVMTraceCPU::tick> tickEvent;

  /*******************************************************************/
  // All the statics.
  /*******************************************************************/
  void regStats() override;
};

#endif
