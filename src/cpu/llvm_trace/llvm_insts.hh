#ifndef __CPU_LLVM_INST_HH__
#define __CPU_LLVM_INST_HH__

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/misc.hh"
#include "base/types.hh"
#include "cpu/op_class.hh"

class LLVMAcceleratorContext;
class LLVMTraceCPU;

using LLVMDynamicInstId = uint64_t;

class LLVMDynamicInst {
 public:
  LLVMDynamicInst(LLVMDynamicInstId _id, const std::string& _instName,
                  std::vector<LLVMDynamicInstId>&& _dependentInstIds)
      : id(_id),
        instName(_instName),
        dependentInstIds(std::move(_dependentInstIds)),
        fuStatus(FUStatus::COMPLETED) {}

  // Interface.
  virtual void execute(LLVMTraceCPU* cpu) = 0;
  virtual std::string toLine() const = 0;

  // Handle a packet response. Default will panic.
  // Only for mem insts.
  virtual void handlePacketResponse() {
    panic("Calling handlePacketResponse on non-mem inst %u\n", id);
  }

  // Hack: get the accelerator context ONLY for Accelerator inst.
  virtual LLVMAcceleratorContext* getAcceleratorContext() {
    panic("Calling getAcceleratorContext on non-accelerator inst %u\n", id);
    return nullptr;
  }

  // Handle a fu completion for next cycle.
  virtual void handleFUCompletion();

  // Handle a tick for inst.
  // This will handle FUCompletion.
  virtual void tick() {
    if (this->fuStatus == FUStatus::COMPLETE_NEXT_CYCLE) {
      this->fuStatus = FUStatus::COMPLETED;
    }
  }

  virtual bool isCompleted() const = 0;

  // Check if all the dependence are ready.
  bool isDependenceReady(LLVMTraceCPU* cpu) const;

  // Get FUs to execute this instruction.
  // Gem5 will break the inst into micro-ops, each micro-op require
  // only one FU.
  // However, some LLVM insts requires more than one FU to execute,
  // e.g. getelementptr need IntMul and IntAlu.
  // TODO: Either to support multiple FUs or break LLVM inst into micro-ops.
  OpClass getOpClass() const;

  // Start the fuStatus state machine.
  // If we need a fu, it will change to WORKING.
  // Otherwise, should remain COMPLETED.
  void startFUStatusFSM();

  // Hack, special interface for call stack inc/dec.
  virtual int getCallStackAdjustment() const { return 0; }

 protected:
  LLVMDynamicInstId id;
  std::string instName;
  std::vector<LLVMDynamicInstId> dependentInstIds;

  // A simple state machine to monitor FU status.
  // We need complete_next_cycle to make sure that latency
  // of FU is precise, as we can't be sure when an
  // FUCompletion is executed in one cycle.
  enum FUStatus {
    WORKING,
    COMPLETE_NEXT_CYCLE,
    COMPLETED,
  } fuStatus;

  // A static global map from instName to the needed OpClass.
  static std::unordered_map<std::string, OpClass> instToOpClass;
};

// Memory access inst.
class LLVMDynamicInstMem : public LLVMDynamicInst {
 public:
  enum Type {
    ALLOCA,
    STORE,
    LOAD,
  };
  LLVMDynamicInstMem(LLVMDynamicInstId _id, const std::string& _instName,
                     std::vector<LLVMDynamicInstId>&& _dependentInstIds,
                     Addr _size, const std::string& _base, Addr _offset,
                     Addr _trace_vaddr, Addr _align, Type _type,
                     uint8_t* _value)
      : LLVMDynamicInst(_id, _instName, std::move(_dependentInstIds)),
        size(_size),
        base(_base),
        offset(_offset),
        trace_vaddr(_trace_vaddr),
        align(_align),
        type(_type),
        value(_value) {}

  void execute(LLVMTraceCPU* cpu) override;

  std::string toLine() const override {
    std::stringstream ss;
    ss << "mem," << this->type << ',';
    ss << this->size << ',';
    ss << this->base << ',';
    ss << std::hex << this->offset << ',';
    ss << this->trace_vaddr << ',';
    ss << std::dec << this->align << ',';
    for (auto id : this->dependentInstIds) {
      ss << id << ',';
    }
    return ss.str();
  }

  void handlePacketResponse() override;

  bool isCompleted() const override {
    return this->fuStatus == FUStatus::COMPLETED && this->numInflyPackets == 0;
  }

 protected:
  Addr size;
  std::string base;
  Addr offset;
  Addr trace_vaddr;
  Addr align;
  Type type;
  // Used for store.
  uint8_t* value;

  // Runtime fields for load/store.
  uint32_t numInflyPackets;
};

class LLVMDynamicInstCompute : public LLVMDynamicInst {
 public:
  enum Type {
    CALL,
    RET,
    SIN,
    COS,
    // Special accelerator inst.
    ACCELERATOR,
    OTHER,
  };
  LLVMDynamicInstCompute(LLVMDynamicInstId _id, const std::string& _instName,
                         std::vector<LLVMDynamicInstId>&& _dependentInstIds,
                         Type _type, LLVMAcceleratorContext* _context)
      : LLVMDynamicInst(_id, _instName, std::move(_dependentInstIds)),
        type(_type),
        context(_context) {}
  void execute(LLVMTraceCPU* cpu) override {}
  std::string toLine() const override {
    std::stringstream ss;
    ss << "com," << this->type << ',';
    for (auto id : this->dependentInstIds) {
      ss << id << ',';
    }
    return ss.str();
  }

  LLVMAcceleratorContext* getAcceleratorContext() override {
    if (this->type != Type::ACCELERATOR) {
      return LLVMDynamicInst::getAcceleratorContext();
    }
    return this->context;
  }

  // FU FSM should be finished.
  bool isCompleted() const override {
    return this->fuStatus == FUStatus::COMPLETED;
  }

  // Adjust the call stack for call/ret inst.
  int getCallStackAdjustment() const override {
    switch (this->type) {
      case Type::CALL:
        return 1;
      case Type::RET:
        return -1;
      default:
        return 0;
    }
  }

 protected:
  Type type;

  // Only used for accelerator inst.
  LLVMAcceleratorContext* context;
};

std::shared_ptr<LLVMDynamicInst> parseLLVMDynamicInst(LLVMDynamicInstId id,
                                                      const std::string& line);

#endif
