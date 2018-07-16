#ifndef __CPU_TDG_ACCELERATOR_ADFA_HH__
#define __CPU_TDG_ACCELERATOR_ADFA_HH__

#include "insts.hh"

#include "base/statistics.hh"
#include "cpu/llvm_trace/accelerator/tdg_accelerator.hh"

#include <list>
#include <unordered_map>

/**
 * Implement the abstract data flow accelerator.
 */

class DynamicInstructionStream;

class AbstractDataFlowAccelerator : public TDGAccelerator {
public:
  AbstractDataFlowAccelerator();
  ~AbstractDataFlowAccelerator() override;

  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void regStats() override;

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
  union {
    ADFAConfigInst *config;
    ADFAStartInst *start;
  } currentInst;
  enum {
    NONE,
    CONFIG,
    START,
  } handling;

  unsigned issueWidth;
  unsigned robSize;

  // Configure overhead;
  int configOverheadInCycles;

  void tickConfig();
  void tickStart();

  DynamicInstructionStream *dataFlow;

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

  // Huge ROB.
  std::list<LLVMDynamicInstId> rob;
  std::list<LLVMDynamicInstId> readyInsts;

  // Flag indicating whether we have encountered the end token.
  LLVMDynamicInst *endToken;

  void fetch();
  void markReady();
  void issue();
  void commit();
  void release();
};

#endif