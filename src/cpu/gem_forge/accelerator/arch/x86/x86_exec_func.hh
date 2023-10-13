
#ifndef __GEM_FORGE_X86_EXEC_FUNC_HH__
#define __GEM_FORGE_X86_EXEC_FUNC_HH__

#include "config/have_protobuf.hh"
#include <vector>
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif
#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "arch/x86/pcstate.hh"
#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"

#include <array>
#include <iostream>

namespace gem5 {

class GemForgeISAHandler;
namespace X86ISA {
class ExecFunc {
public:
  /**
   * We make the register value large enough to hold AVX-512.
   */
  static constexpr int MaxRegisterValueSize = 128;
  using DataType = ::LLVM::TDG::DataType;
  class RegisterValue : public std::array<uint64_t, MaxRegisterValueSize> {
  public:
    RegisterValue() : std::array<uint64_t, MaxRegisterValueSize>() {
      this->fill(0);
    }
    std::string print(const DataType &type) const;
    std::string print() const;
    uint8_t *uint8Ptr(int offset = 0) {
      assert(offset < MaxRegisterValueSize * sizeof(uint64_t));
      return reinterpret_cast<uint8_t *>(&this->front()) + offset;
    }
    const uint8_t *uint8Ptr(int offset = 0) const {
      assert(offset < MaxRegisterValueSize * sizeof(uint64_t));
      return reinterpret_cast<const uint8_t *>(&this->front()) + offset;
    }
    const uint64_t &uint64(int offset = 0) const {
      assert(offset < MaxRegisterValueSize);
      return this->at(offset);
    }
    uint64_t &uint64(int offset = 0) {
      assert(offset < MaxRegisterValueSize);
      return this->at(offset);
    }
    int64_t int64(int offset = 0) const {
      assert(offset < MaxRegisterValueSize);
      return this->at(offset);
    }
  };
  static int translateToNumRegs(const DataType &type);
  static std::string printRegisterValue(const RegisterValue &value,
                                        const DataType &type);

  ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func);

  RegisterValue invoke(const std::vector<RegisterValue> &params,
                       GemForgeISAHandler *isaHandler = nullptr,
                       InstSeqNum startSeqNum = 0);

  /**
   * Exposes the values of each instruction's output register.
   */
  std::vector<RegisterValue>
  invoke_imvals(const std::vector<RegisterValue> &params,
                GemForgeISAHandler *isaHandler = nullptr,
                InstSeqNum startSeqNum = 0);

  /**
   * A special interface for address generation. Mostly used for indirect
   * streams.
   */
  Addr invoke(const std::vector<Addr> &params);

  Cycles getEstimatedLatency() const { return this->estimatedLatency; }
  const ::LLVM::TDG::ExecFuncInfo &getFuncInfo() const { return this->func; }
  bool hasSIMD() const { return this->isSIMD; }
  bool hasSIMDMatrix() const { return this->isSIMDMatrix; }

  int getNumInstructions() const { return this->instructions.size(); }
  int getNumInstsBeforeStreamConfig() const {
    return this->numInstsBeforeStreamConfig;
  }
  bool isNop() const { return this->instructions.empty(); }
  const std::vector<StaticInstPtr> &getStaticInsts() const {
    return this->instructions;
  }
  Cycles getLastInstLat() const {
    assert(!this->instructions.empty());
    return this->estimateOneInstLat(this->instructions.back());
  }

private:
  ThreadContext *tc;
  const ::LLVM::TDG::ExecFuncInfo &func;
  // All the types are integer.
  bool isPureInteger;
  bool isSIMD = false;
  bool isSIMDMatrix = false;
  Cycles estimatedLatency;

  int numInstsBeforeStreamConfig = 0;
  std::vector<StaticInstPtr> instructions;
  std::vector<X86ISA::PCState> pcs;

  void estimateLatency();
  Cycles estimateOneInstLat(const StaticInstPtr &staticInst) const;
};

std::ostream &operator<<(std::ostream &os,
                         const X86ISA::ExecFunc::RegisterValue &value);

} // namespace X86ISA

} // namespace gem5

#endif
