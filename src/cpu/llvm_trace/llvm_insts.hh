#ifndef __CPU_LLVM_INST_HH__
#define __CPU_LLVM_INST_HH__

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "base/misc.hh"
#include "base/types.hh"

class LLVMTraceCPU;

using LLVMDynamicInstId = uint64_t;

class LLVMDynamicInst {
 public:
  LLVMDynamicInst(LLVMDynamicInstId _id,
                  std::vector<LLVMDynamicInstId>&& _dependentInstIds)
      : id(_id), dependentInstIds(std::move(_dependentInstIds)) {}

  // Interface.
  virtual void execute(LLVMTraceCPU* cpu) = 0;
  virtual std::string toLine() const = 0;

  // Handle a packet response. Default will panic.
  // Only for mem insts.
  virtual void handlePacketResponse() {
    panic("Calling handlePacketResponse on non-mem inst %u\n", id);
  }
  // Handle a tick for inst.
  virtual void tick() = 0;

  virtual bool isCompleted() const = 0;

  // Check if all the dependence are ready.
  bool isDependenceReady(LLVMTraceCPU* cpu) const;

  // Hack, special interface for call stack inc/dec.
  virtual int getCallStackAdjustment() const { return 0; }

 protected:
  LLVMDynamicInstId id;
  std::vector<LLVMDynamicInstId> dependentInstIds;
};

// Memory access inst.
class LLVMDynamicInstMem : public LLVMDynamicInst {
 public:
  enum Type {
    ALLOCA,
    STORE,
    LOAD,
  };
  LLVMDynamicInstMem(LLVMDynamicInstId _id,
                     std::vector<LLVMDynamicInstId>&& _dependentInstIds,
                     Addr _size, const std::string& _base, Addr _offset,
                     Addr _trace_vaddr, Addr _align, Type _type,
                     uint8_t* _value)
      : LLVMDynamicInst(_id, std::move(_dependentInstIds)),
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
  // Handle a tick for inst.
  void tick() override {
    // Silently ignore tick event.
  }

  bool isCompleted() const override { return this->numInflyPackets == 0; }

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
    OTHER,
  };
  LLVMDynamicInstCompute(LLVMDynamicInstId _id,
                         std::vector<LLVMDynamicInstId>&& _dependentInstIds,
                         Tick _computeDelay, Type _type)
      : LLVMDynamicInst(_id, std::move(_dependentInstIds)),
        computeDelay(_computeDelay),
        type(_type) {}
  void execute(LLVMTraceCPU* cpu) override {
    // For now just set the remainTicks.
    this->remainTicks = this->computeDelay;
  }
  std::string toLine() const override {
    std::stringstream ss;
    ss << "com," << this->type << ',';
    ss << this->computeDelay << ',';
    for (auto id : this->dependentInstIds) {
      ss << id << ',';
    }
    return ss.str();
  }
  // Handle a tick for inst.
  void tick() override { this->remainTicks--; }

  bool isCompleted() const override { return remainTicks == 0; }

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
  Tick computeDelay;
  Type type;
  // Runtime fields.
  Tick remainTicks;
};

std::shared_ptr<LLVMDynamicInst> parseLLVMDynamicInst(LLVMDynamicInstId id,
                                                      const std::string& line);

#endif
