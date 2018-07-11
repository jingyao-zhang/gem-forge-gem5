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
#include "mem/packet.hh"

class LLVMAcceleratorContext;
class LLVMTraceCPU;

using LLVMDynamicInstId = uint64_t;

class LLVMDynamicInst {
public:
  LLVMDynamicInst(LLVMDynamicInstId _id, const std::string &_instName,
                  uint8_t _numMicroOps,
                  std::vector<LLVMDynamicInstId> &&_dependentInstIds)
      : id(_id), instName(_instName), numMicroOps(_numMicroOps),
        dependentInstIds(std::move(_dependentInstIds)), staticInstAddress(0x0),
        nextBBName(""), fuStatus(FUStatus::COMPLETED),
        remainingMicroOps(_numMicroOps - 1) {}

  virtual ~LLVMDynamicInst() {}

  // Interface.
  virtual void execute(LLVMTraceCPU *cpu) = 0;
  virtual void writeback(LLVMTraceCPU *cpu) {
    panic("Calling write back on non-store inst %u.\n", this->id);
  }
  virtual std::string toLine() const = 0;

  // Handle a packet response. Default will panic.
  // Only for mem insts.
  virtual void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) {
    panic("Calling handlePacketResponse on non-mem inst %u\n", id);
  }

  // Hack: get the accelerator context ONLY for Accelerator inst.
  virtual LLVMAcceleratorContext *getAcceleratorContext() {
    panic("Calling getAcceleratorContext on non-accelerator inst %u\n", id);
    return nullptr;
  }

  // Handle a fu completion for next cycle.
  virtual void handleFUCompletion();

  // Handle a tick for inst.
  // This will handle FUCompletion.
  virtual void tick() {
    if (this->fuStatus == FUStatus::COMPLETED) {
      // The fu is completed.
      if (this->remainingMicroOps > 0) {
        this->remainingMicroOps--;
      }
    }
    if (this->fuStatus == FUStatus::COMPLETE_NEXT_CYCLE) {
      this->fuStatus = FUStatus::COMPLETED;
    }
  }

  virtual bool isCompleted() const = 0;
  virtual bool isWritebacked() const {
    panic("Calling isWritebacked on non-store inst %u.\n", id);
    return false;
  }

  bool isBranchInst() const;
  bool isConditionalBranchInst() const;
  bool isStoreInst() const;
  bool isLoadInst() const;

  // Check if all the dependence are ready.
  bool isDependenceReady(LLVMTraceCPU *cpu) const;

  // Check if this inst can write back. It checks
  // for control dependence is committed.
  virtual bool canWriteBack(LLVMTraceCPU *cpu) const;

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

  const std::string &getInstName() const { return instName; }
  LLVMDynamicInstId getId() const { return id; }

  uint8_t getNumMicroOps() const { return numMicroOps; }
  uint8_t getQueueWeight() const { return numMicroOps; }

  uint64_t getStaticInstAddress() const { return staticInstAddress; }
  void setStaticInstAddress(uint64_t staticInstAddress) {
    this->staticInstAddress = staticInstAddress;
  }

  const std::string &getNextBBName() const { return nextBBName; }
  void setNextBBName(const std::string &nextBBName) {
    this->nextBBName = nextBBName;
  }

protected:
  LLVMDynamicInstId id;
  std::string instName;
  uint8_t numMicroOps;
  std::vector<LLVMDynamicInstId> dependentInstIds;

  // Used only for conditional branch.
  uint64_t staticInstAddress;
  std::string nextBBName;

  // A simple state machine to monitor FU status.
  // We need complete_next_cycle to make sure that latency
  // of FU is precise, as we can't be sure when an
  // FUCompletion is executed in one cycle.
  enum FUStatus {
    WORKING,
    COMPLETE_NEXT_CYCLE,
    COMPLETED,
  } fuStatus;

  // Runtime value to simulate depedendence of micro ops
  // inside an instruction.
  // For any remaining micro ops, we delay the completion
  // for 1 tick. This is really coarse-grained.
  uint8_t remainingMicroOps;

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
  LLVMDynamicInstMem(LLVMDynamicInstId _id, const std::string &_instName,
                     uint8_t _numMicroOps,
                     std::vector<LLVMDynamicInstId> &&_dependentInstIds,
                     Addr _size, const std::string &_base, Addr _offset,
                     Addr _trace_vaddr, Addr _align, Type _type,
                     uint8_t *_value, const std::string &_new_base)
      : LLVMDynamicInst(_id, _instName, _numMicroOps,
                        std::move(_dependentInstIds)),
        size(_size), base(_base), offset(_offset), trace_vaddr(_trace_vaddr),
        align(_align), type(_type), value(_value), new_base(_new_base) {
    if (this->type == ALLOCA) {
      if (this->new_base == "") {
        panic("Alloc with empty new base.\n");
      }
    }
  }

  ~LLVMDynamicInstMem() override;

  void execute(LLVMTraceCPU *cpu) override;

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

  void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;

  bool isCompleted() const override;

  bool isWritebacked() const override;
  void writeback(LLVMTraceCPU *cpu) override;

  // bool canWriteBack(LLVMTraceCPU* cpu) const override;

protected:
  Addr size;
  std::string base;
  Addr offset;
  Addr trace_vaddr;
  Addr align;
  Type type;
  // Used for store.
  uint8_t *value;
  // Used for load.
  std::string new_base;

  // Runtime fields for load/store.
  struct PacketParam {
  public:
    Addr paddr;
    int size;
    uint8_t *data;
    PacketParam(Addr _paddr, int _size, uint8_t *_data)
        : paddr(_paddr), size(_size), data(_data) {}
  };
  std::list<PacketParam> packets;

  void constructPackets(LLVMTraceCPU *cpu);
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
  LLVMDynamicInstCompute(LLVMDynamicInstId _id, const std::string &_instName,
                         uint8_t _numMicroOps,
                         std::vector<LLVMDynamicInstId> &&_dependentInstIds,
                         Type _type, LLVMAcceleratorContext *_context)
      : LLVMDynamicInst(_id, _instName, _numMicroOps,
                        std::move(_dependentInstIds)),
        type(_type), context(_context) {}
  void execute(LLVMTraceCPU *cpu) override {}
  std::string toLine() const override {
    std::stringstream ss;
    ss << "com," << this->type << ',';
    for (auto id : this->dependentInstIds) {
      ss << id << ',';
    }
    return ss.str();
  }

  LLVMAcceleratorContext *getAcceleratorContext() override {
    if (this->type != Type::ACCELERATOR) {
      return LLVMDynamicInst::getAcceleratorContext();
    }
    return this->context;
  }

  // FU FSM should be finished.
  bool isCompleted() const override {
    return this->fuStatus == FUStatus::COMPLETED &&
           this->remainingMicroOps == 0;
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
  LLVMAcceleratorContext *context;
};

#endif
