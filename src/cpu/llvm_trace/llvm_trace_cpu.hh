#ifndef __CPU_LLVM_TRACE_CPU_HH__
#define __CPU_LLVM_TRACE_CPU_HH__

#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <utility>
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

  using DynamicInstId = uint64_t;

  struct DynamicInst {
    enum Type : uint8_t {
      LOAD,
      STORE,
      COMPUTE,
    } type;
    Tick computeDelay;
    std::vector<DynamicInstId> dependentInstIds;
    // Used for LOAD/STORE.
    int size;
    std::string base;
    Addr offset;
    Addr trace_vaddr;  // Trace space vaddr.
    // Used for STORE.
    void* value;
    DynamicInst(Type _type, Tick _computeDelay,
                std::vector<DynamicInstId>&& _dependentInstIds, int _size,
                const std::string& _base, Addr _offset = 0x0,
                Addr _trace_vaddr = 0x0, void* _value = nullptr)
        : type(_type),
          computeDelay(_computeDelay),
          dependentInstIds(std::move(_dependentInstIds)),
          size(_size),
          base(_base),
          offset(_offset),
          trace_vaddr(_trace_vaddr),
          value(_value) {}
    // Print to a one line string (without '\n' ending).
    std::string toLine() const {
      std::stringstream ss;
      ss << this->type << ',' << this->computeDelay << ',';
      if (this->type == Type::LOAD || this->type == Type::STORE) {
        ss << this->size << ',' << this->base << ',' << std::hex << this->offset
           << ',' << this->trace_vaddr << ',' << std::dec;
      }
      for (auto id : this->dependentInstIds) {
        ss << id << ',';
      }
      return ss.str();
    }
  };

  /**
   * Read in the trace file and popluate dynamicInsts vector.
   */
  void readTraceFile();

  void tick();

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

  //*********************************************************//
  // A map from base name to user space address.
  std::unordered_map<std::string, Addr> mapBaseToVAddr;

  // Process and ThreadContext for the simulation program.
  Process* process;
  ThreadContext* thread_context;

  Addr finish_tag_paddr;

  //********************************************************//
  EventWrapper<LLVMTraceCPU, &LLVMTraceCPU::tick> tickEvent;

  enum InstStatus {
    FETCHED,  // In fetchQueue.
    READY,    // Ready to be issued.
    ISSUED,
    FINISHED,  // Finished computing.
  };
  std::unordered_map<DynamicInstId, InstStatus> inflyInsts;

  std::queue<DynamicInstId> fetchQueue;
  const size_t MAX_FETCH_QUEUE_SIZE;

  void fetch();

  // reorder buffer.
  std::vector<std::pair<DynamicInstId, bool>> rob;
  const size_t MAX_REORDER_BUFFER_SIZE;

  // Fetch instructions from fetchQueue and fill into rob.
  void reorder();

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
};

#endif
