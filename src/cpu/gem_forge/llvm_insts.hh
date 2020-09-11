#ifndef __CPU_LLVM_INST_HH__
#define __CPU_LLVM_INST_HH__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "TDGInstruction.pb.h"

#include "gem_forge_packet_handler.hh"

// #include "base/misc.hh""
#include "base/types.hh"
#include "cpu/op_class.hh"
#include "cpu/static_inst_fwd.hh"
#include "lsq.hh"
#include "mem/packet.hh"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class LLVMTraceCPU;
class LLVMStaticInst;
class LLVMDynamicInst;

struct DynamicInstructionStreamPacket {
  LLVMDynamicInst *inst;
  LLVM::TDG::TDGInstruction tdg;
  bool released;
  DynamicInstructionStreamPacket() : inst(nullptr), released(false) {}
};

using LLVMDynamicInstId = uint64_t;

struct LLVMInstInfo {
public:
  OpClass opClass;
  int numOperands;
  int numResults;
};

class LLVMDynamicInst : public GemForgePacketHandler {
public:
  LLVMDynamicInst(const LLVM::TDG::TDGInstruction &_TDG, uint8_t _numMicroOps)
      : seqNum(allocateGlobalSeqNum()), TDG(_TDG), numMicroOps(_numMicroOps),
        remainingMicroOps(_numMicroOps - 1), serializeBefore(false),
        numAdditionalLQCallbacks(0) {
    // Extrace used stream ids.
    for (const auto &dep : this->TDG.deps()) {
      if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::STREAM) {
        this->usedStreamIds.push_back(dep.dependent_id());
      }
    }
  }

  virtual ~LLVMDynamicInst() {}

  // Interface.
  virtual bool canDispatch(LLVMTraceCPU *cpu) const { return true; }
  virtual void dispatch(LLVMTraceCPU *cpu);
  virtual void execute(LLVMTraceCPU *cpu) = 0;
  virtual void writeback(LLVMTraceCPU *cpu) {
    panic("Calling write back on non-store inst %u.\n", this->getId());
  }
  virtual void commit(LLVMTraceCPU *cpu);

  // Handle a packet response. Default will panic.
  // Only for mem insts.
  virtual void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                                    PacketPtr packet) override {
    panic("Calling handlePacketResponse on non-mem inst %u\n", this->getId());
  }

  // Do nothing.
  void issueToMemoryCallback(GemForgeCPUDelegator *cpuDelegator) override {}

  // Handle a tick for inst. By default do nothing.
  virtual void tick() {}

  virtual bool isCompleted() const = 0;
  virtual bool isWritebacked() const {
    panic("Calling isWritebacked on non-store inst %u.\n", this->getId());
    return false;
  }

  bool isBranchInst() const;
  bool isStoreInst() const;
  bool isLoadInst() const;

  /**
   * Helper functions for general stream user inst.
   */
  std::vector<uint64_t> usedStreamIds;
  bool hasStreamUse() const;
  void dispatchStreamUser(LLVMTraceCPU *cpu);
  void executeStreamUser(LLVMTraceCPU *cpu);
  void commitStreamUser(LLVMTraceCPU *cpu);

  /**
   * Handle serialization instructions.
   * A SerializeAfter instruction will mark the next instruction
   * SerializeBefore.
   *
   * A SerializeBefore instruction will not be dispatched until
   * it reaches the head of rob.
   */
  virtual bool isSerializeAfter() const { return false; }
  virtual bool isSerializeBefore() const { return this->serializeBefore; }
  void markSerializeBefore() { this->serializeBefore = true; }

  // Check if all the dependence are ready.
  virtual bool isDependenceReady(LLVMTraceCPU *cpu) const;

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

  // Get the number of LQ entries required for this instruction.
  int getNumLQEntries(LLVMTraceCPU *cpu) const;
  virtual int getNumSQEntries(LLVMTraceCPU *cpu) const;

  // Get additional LQ callbacks, except the normal load inst.
  void createAdditionalLQCallbacks(LLVMTraceCPU *cpu,
                                   GemForgeLQCallbackList &callbacks);
  int getNumAdditionalLQCallbacks() const {
    return this->numAdditionalLQCallbacks;
  }

  // Get additional SQ callbacks, except the normal store inst.
  virtual std::list<std::unique_ptr<GemForgeSQCallback>>
  createAdditionalSQCallbacks(LLVMTraceCPU *cpu) {
    return std::list<std::unique_ptr<GemForgeSQCallback>>();
  }

  int getNumOperands() const;
  int getNumResults() const;
  bool isFloatInst() const;
  bool isCallInst() const;

  // Hack, special interface for call stack inc/dec.
  virtual int getCallStackAdjustment() const { return 0; }

  /**
   * Getters.
   */
  static constexpr uint64_t INVALID_SEQ_NUM = 0;
  static constexpr LLVMDynamicInstId INVALID_INST_ID = 0;
  uint64_t getSeqNum() const { return this->seqNum; }
  const LLVM::TDG::TDGInstruction &getTDG() const { return this->TDG; }

  const std::string &getInstName() const { return this->TDG.op(); }
  LLVMDynamicInstId getId() const { return this->TDG.id(); }

  uint8_t getNumMicroOps() const { return numMicroOps; }
  uint8_t getQueueWeight() const { return numMicroOps; }

  uint64_t getPC() const { return this->TDG.pc(); }
  uint64_t getDynamicNextPC() const;
  uint64_t getStaticNextPC() const;

  StaticInstPtr getStaticInst() const;

  virtual void dumpBasic() const;
  virtual void dumpDeps(LLVMTraceCPU *cpu) const;

  /**
   * Helper function to allocate a instruction id at run time.
   * Used for dynamic create new instructions.
   * To avoid conflict, this id will start from a large number, (1e10).
   */
  static LLVMDynamicInstId allocateDynamicInstId() {
    return ++currentDynamicInstId;
  }

protected:
  /**
   * An incontinuous sequence number.
   */
  const uint64_t seqNum;
  const LLVM::TDG::TDGInstruction &TDG;

  uint8_t numMicroOps;

  // Runtime value to simulate depedendence of micro ops
  // inside an instruction.
  // For any remaining micro ops, we delay the completion
  // for 1 tick. This is really coarse-grained.
  uint8_t remainingMicroOps;

  bool serializeBefore;

  int numAdditionalLQCallbacks;

  // A static global map from instName to the needed OpClass.
  static std::unordered_map<std::string, LLVMInstInfo> instInfo;

  static LLVMDynamicInstId currentDynamicInstId;
  static InstSeqNum currentSeqNum;
  static InstSeqNum allocateGlobalSeqNum();

public:
  static InstSeqNum getGlobalSeqNum();
  static void setGlobalSeqNum(InstSeqNum seqNum);
};

// Memory access inst.
class LLVMDynamicInstMem : public LLVMDynamicInst {
public:
  enum Type {
    ALLOCA,
    STORE,
    LOAD,
  };
  LLVMDynamicInstMem(const LLVM::TDG::TDGInstruction &_TDG,
                     uint8_t _numMicroOps, Addr _align, Type _type);

  ~LLVMDynamicInstMem() override;

  void execute(LLVMTraceCPU *cpu) override;

  void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                            PacketPtr packet) override;

  bool isCompleted() const override;

  bool isWritebacked() const override;
  void writeback(LLVMTraceCPU *cpu) override;

  void dumpBasic() const override;

  // bool canWriteBack(LLVMTraceCPU* cpu) const override;

protected:
  Addr align;
  Type type;
  // For store only.
  uint8_t *value;

  // For load profile.
  uint64_t loadStartCycle;
  uint64_t loadEndCycle;

  // Runtime fields for load/store.
  struct PacketParam {
  public:
    Addr paddr;
    Addr vaddr;
    int size;
    uint8_t *data;
    PacketParam(Addr _paddr, Addr _vaddr, int _size, uint8_t *_data)
        : paddr(_paddr), vaddr(_vaddr), size(_size), data(_data) {}
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
  LLVMDynamicInstCompute(const LLVM::TDG::TDGInstruction &_TDG,
                         uint8_t _numMicroOps, Type _type)
      : LLVMDynamicInst(_TDG, _numMicroOps), type(_type) {}
  void execute(LLVMTraceCPU *cpu) override;

  void tick() override {
    if (this->fuLatency > Cycles(0)) {
      --this->fuLatency;
    }
  }

  // FU FSM should be finished.
  bool isCompleted() const override { return this->fuLatency == Cycles(0); }

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
  Cycles fuLatency;
};

#endif
