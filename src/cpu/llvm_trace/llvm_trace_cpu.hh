#ifndef __CPU_LLVM_TRACE_CPU_HH__
#define __CPU_LLVM_TRACE_CPU_HH__

#include <fstream>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tdg_packet_handler.hh"

#include "base/statistics.hh"
#include "cpu/base.hh"
#include "cpu/llvm_trace/accelerator/tdg_accelerator.hh"
#include "cpu/llvm_trace/dyn_inst_stream.hh"
#include "cpu/llvm_trace/llvm_commit_stage.hh"
#include "cpu/llvm_trace/llvm_decode_stage.hh"
#include "cpu/llvm_trace/llvm_fetch_stage.hh"
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_insts.hh"
#include "cpu/llvm_trace/llvm_rename_stage.hh"
#include "cpu/llvm_trace/llvm_stage_signal.hh"
#include "cpu/llvm_trace/llvm_trace_cpu_driver.hh"
#include "cpu/llvm_trace/region_stats.hh"
#include "cpu/o3/fu_pool.hh"
#include "mem/page_table.hh"
#include "params/LLVMTraceCPU.hh"

class LLVMTraceCPU : public BaseCPU {
public:
  LLVMTraceCPU(LLVMTraceCPUParams *params);
  ~LLVMTraceCPU();

  MasterPort &getDataPort() override { return this->dataPort; }

  MasterPort &getInstPort() override { return this->instPort; }

  Counter totalInsts() const override { return 0; }

  Counter totalOps() const override { return 0; }

  void wakeup(ThreadID tid) override {}

  bool handleTimingResp(PacketPtr pkt);

  // API interface for driver.
  void handleReplay(Process *p, ThreadContext *tc, const std::string &trace,
                    const Addr finish_tag_vaddr,
                    std::vector<std::pair<std::string, Addr>> maps);

  enum InstStatus {
    FETCHED,    // In fetchQueue.
    DECODED,    // Decoded.
    DISPATCHED, // Dispatched to instQueue.
    READY,      // Ready to be issued.
    ISSUED,
    FINISHED,     // Finished computing.
    COMMIT,       // Sent to commit stage.
    COMMITTING,   // Committing.
    COMMITTED,    // Committed.
    WRITEBACKING, // Writing back.
    WRITEBACKED,  // Write backed.
  };

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
    void sendReq();
    void addReq(PacketPtr pkt);
    void recvReqRetry() override;

    size_t getPendingPacketsNum() const {
      return this->blockedPacketPtrs.size();
    }

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
  };

  void tick();

  FuncPageTable pageTable;
  CPUPort instPort;
  CPUPort dataPort;

  const std::string traceFileName;
  DynamicInstructionStream *dynInstStream;

  TheISA::TLB *itb;
  TheISA::TLB *dtb;

  FUPool *fuPool;

  RegionStats *regionStats;

  // Used to record the current stack depth, so that we can break trace
  // into multiple function calls.
  int currentStackDepth;
  std::list<Addr> framePointerStack;
  void stackPop();
  void stackPush();

  // Cache warm up in standalone mode.
  void warmUpCache(const std::string &FileName);

  // Load more dynamic instructions if needed.
  void loadDynamicInstsIfNecessary();

  // In fly instructions.
  std::unordered_map<LLVMDynamicInstId, LLVMDynamicInst *> inflyInstMap;

  // The status of infly instructions.
  std::unordered_map<LLVMDynamicInstId, InstStatus> inflyInstStatus;

  //*********************************************************//
  // This variables are initialized per replay.
  // A map from base name to user space address.
  std::unordered_map<std::string, Addr> mapBaseToVAddr;

  // Process and ThreadContext for the simulation program.
  Process *process;
  ThreadContext *thread_context;
  SymbolTable symbol_table;
  // The top of the stack for this replay.
  Addr stackMin;

  Addr finish_tag_paddr;

  friend class LLVMFetchStage;
  friend class LLVMDecodeStage;
  friend class LLVMRenameStage;
  friend class LLVMIEWStage;
  friend class LLVMCommitStage;
  friend class TDGLoadStoreQueue;

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
  TDGAcceleratorManager *accelManager;
  TDGAcceleratorManagerParams *accelManagerParams;

  /**************************************************************/
  // Interface for the insts.
public:
  Stats::Distribution numPendingAccessDist;

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
  // If data is not nullptr, it will be a write.
  void sendRequest(Addr paddr, int size, TDGPacketHandler *handler,
                   uint8_t *data);

  Cycles getOpLatency(OpClass opClass);

  TDGAcceleratorManager *getAcceleratorManager() { return this->accelManager; }

  void updateBasicBlock(const RegionStats::BasicBlockId &bb) {
    this->regionStats->update(bb);
  }

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
