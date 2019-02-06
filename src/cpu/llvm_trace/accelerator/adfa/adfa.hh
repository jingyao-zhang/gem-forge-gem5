#ifndef __CPU_TDG_ACCELERATOR_ADFA_HH__
#define __CPU_TDG_ACCELERATOR_ADFA_HH__

#include "insts.hh"

#include "base/statistics.hh"
#include "cpu/llvm_trace/accelerator/tdg_accelerator.hh"
#include "cpu/llvm_trace/bank_manager.hh"

#include <list>
#include <unordered_map>

/**
 * Implement the abstract data flow accelerator.
 */

class DynamicInstructionStream;
class DynamicInstructionStreamInterface;

class AbstractDataFlowCore {
public:
  AbstractDataFlowCore(const std::string &_id, LLVMTraceCPU *_cpu);
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
  Stats::Distribution numCommittedDist;
  Stats::Scalar numConfigured;
  Stats::Scalar numExecution;
  Stats::Scalar numCycles;
  Stats::Scalar numCommittedInst;

private:
  std::string id;
  LLVMTraceCPU *cpu;

  bool busy;
  DynamicInstructionStreamInterface *dataFlow;

  bool enableSpeculation;
  bool breakIVDep;
  bool breakRVDep;
  unsigned numBanks;
  unsigned numPortsPerBank;

  unsigned issueWidth;
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

  // Huge ROB.
  std::list<LLVMDynamicInstId> rob;
  std::list<LLVMDynamicInstId> readyInsts;

  void fetch();
  void markReady();
  void issue();
  void commit();
  void release();
};

class AbstractDataFlowAccelerator : public TDGAccelerator {
public:
  AbstractDataFlowAccelerator();
  ~AbstractDataFlowAccelerator() override;

  void handshake(LLVMTraceCPU *_cpu, TDGAcceleratorManager *_manager) override;
  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void dump() override;
  void regStats() override;

  /**
   * Stats
   */

  Stats::Scalar numConfigured;
  Stats::Scalar numExecution;
  Stats::Scalar numCycles;
  Stats::Scalar numCommittedInst;

private:
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
    bool shouldSerialize;
    Job() : dataFlow(nullptr), core(nullptr), shouldSerialize(false) {}
  };

  std::list<Job> pendingJobs;
  std::list<Job> workingJobs;

  // Configure overhead;
  int configOverheadInCycles;

  int numCores;
  bool enableTLS;

  void tickConfig();
  void tickStart();

  DynamicInstructionStream *dataFlow;

  std::vector<AbstractDataFlowCore> cores;
};

#endif