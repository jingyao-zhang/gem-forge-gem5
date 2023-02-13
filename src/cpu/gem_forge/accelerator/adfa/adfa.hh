#ifndef __CPU_TDG_ACCELERATOR_ADFA_HH__
#define __CPU_TDG_ACCELERATOR_ADFA_HH__

#include "params/AbstractDataFlowAccelerator.hh"

#include "insts.hh"

#include "base/statistics.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/gem_forge/bank_manager.hh"
#include "cpu/gem_forge/dyn_inst_stream.hh"
#include "cpu/gem_forge/dyn_inst_stream_dispatcher.hh"

#include <list>
#include <unordered_map>

namespace gem5 {

/**
 * Implement the abstract data flow accelerator.
 */

class AbstractDataFlowCore {
public:
  AbstractDataFlowCore(const std::string &_id, LLVMTraceCPU *_cpu,
                       GemForgeCPUDelegator *_cpuDelegator,
                       const AbstractDataFlowAcceleratorParams *params);

  AbstractDataFlowCore(const AbstractDataFlowCore &other) = delete;
  AbstractDataFlowCore(AbstractDataFlowCore &&other) = delete;

  AbstractDataFlowCore &operator=(const AbstractDataFlowCore &other) = delete;
  AbstractDataFlowCore &operator=(AbstractDataFlowCore &&other) = delete;

  ~AbstractDataFlowCore();

  const char *name() const { return this->id.c_str(); }

  void tick();
  void dump();
  void regStats();

  bool isBusy() const { return this->busy; }

  void start(DynamicInstructionStreamInterface *dataFlow);

  /**
   * Stats
   */
  Stats::Distribution numIssuedDist;
  Stats::Distribution numIssuedLoadDist;
  Stats::Distribution numCommittedDist;
  statistics::Scalar numExecution;
  statistics::Scalar numCycles;
  statistics::Scalar numCommittedInst;
  statistics::Scalar numBankConflicts;

private:
  std::string id;
  LLVMTraceCPU *cpu;
  GemForgeCPUDelegator *cpuDelegator;

  bool busy;
  DynamicInstructionStreamInterface *dataFlow;

  bool enableSpeculation;
  bool breakIVDep;
  bool breakRVDep;
  bool breakUnrollableControlDep;
  bool idealMem;
  const int idealMemLatency = 2;
  unsigned numBanks;
  unsigned numPortsPerBank;

  unsigned issueWidth;
  unsigned fetchQueueSize;
  unsigned robSize;

  /**
   * Execution Model:
   * Although this is a data flow accelerator, out implementation still takes a
   * similar centralized approach to GPP to moniter the ready instruction.
   *
   * Instructions are fetched directly into a huge ROB.
   * Each instruction can be one of the following state:
   *    FETCHED:
   *        fetched into the huge rob.
   *        fetched into the infly instruction status map.
   *        set the age of the instruction.
   *    READY:
   *        the instruction is marked as ready and insert into ready list.
   *        the ready list is sorted by the instruction's age.
   *    ISSUED:
   *        the instruction is issued from the ready list.
   *    FINISHED:
   *        the instruction is actually finished, but not commited simply
   *        because our dynamic instruction stream requires committing in order.
   *
   * Note that there is no committed status, as the accelerator is non-specular
   * so we actually allow commit out of order. (really?)
   *
   * The fetch will stop when we encounter an ADFAEndToken.
   * The data flow execution will stop when the infly instruction status map is
   * empty (there is no infly instruction).
   */

  enum InstStatus {
    FETCHED,
    DECODED,
    READY,
    ISSUED,
    FINISHED,
  };

  using Age = uint64_t;
  Age currentAge;
  std::unordered_map<LLVMDynamicInstId, Age> inflyInstAge;
  std::unordered_map<LLVMDynamicInstId, InstStatus> inflyInstStatus;
  std::unordered_map<LLVMDynamicInstId, LLVMDynamicInst *> inflyInstMap;

  BankManager *bankManager;

  // Queues.
  std::list<LLVMDynamicInstId> fetchQueue;
  std::list<LLVMDynamicInstId> rob;
  std::list<LLVMDynamicInstId> readyInsts;

  // This is used to model one-cycle latency ideal memory.
  // Memory instructions will be sent into this queue, and marked finished one
  // cycle later.
  std::list<std::pair<Tick, LLVMDynamicInstId>> idealMemCompleteQueue;

  void fetch();
  void decode();
  void markReady();
  void issue();
  void commit();
  void release();
};

class AbstractDataFlowAccelerator : public GemForgeAccelerator {
public:
  PARAMS(AbstractDataFlowAccelerator)
  AbstractDataFlowAccelerator(const Params &_params);
  ~AbstractDataFlowAccelerator() override;

  void handshake(GemForgeCPUDelegator *_cpuDelegator,
                 GemForgeAcceleratorManager *_manager) override;
  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void dump() override;
  void regStats() override;

  /**
   * Stats
   */

  statistics::Scalar numConfigured;
  statistics::Scalar numExecution;
  statistics::Scalar numCycles;
  statistics::Scalar numTLSJobs;
  statistics::Scalar numTLSJobsSerialized;

private:
  LLVMTraceCPU *cpu;
  union {
    ADFAConfigInst *config;
    ADFAStartInst *start;
  } currentInst;
  enum {
    NONE,
    CONFIG,
    START,
  } handling;

  /**
   * A simple class holding the execution jobs for the cores.
   */
  struct Job {
    DynamicInstructionStreamInterface *dataFlow;
    AbstractDataFlowCore *core;
    std::shared_ptr<std::unordered_set<LLVMDynamicInstId>> instIds;
    bool shouldSerialize;
    uint64_t jobId;
    Job()
        : dataFlow(nullptr), core(nullptr), shouldSerialize(false), jobId(0) {}
  };

  std::list<Job> pendingJobs;
  std::list<Job> workingJobs;

  // Configure overhead;
  int configOverheadInCycles;

  // Configured loop iteration start boundary.
  uint64_t configuredLoopStartPC;
  std::string configuredLoopName;

  DynamicInstructionStream::Iterator TLSLHSIter;
  uint64_t TLSJobId;

  int numCores;
  bool enableTLS;

  DynamicInstructionStream *dataFlow;
  DynamicInstructionStreamDispatcher *dataFlowDispatcher;

  std::vector<AbstractDataFlowCore *> cores;

  void tickConfig();
  void tickStart();

  /**
   * Create one TLS job per loop iteration.
   */
  void createTLSJobs();

  bool isTLSBoundary(LLVMDynamicInst *inst) const;
  bool hasTLSDependence(LLVMDynamicInst *inst) const;
};

} // namespace gem5

#endif