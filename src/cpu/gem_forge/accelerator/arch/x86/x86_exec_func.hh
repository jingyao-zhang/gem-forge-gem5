
#ifndef __GEM_FORGE_X86_EXEC_FUNC_HH__
#define __GEM_FORGE_X86_EXEC_FUNC_HH__

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif
#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "cpu/inst_seq.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"

#include <array>
#include <iostream>

class GemForgeISAHandler;
namespace X86ISA {
class ExecFunc {
public:
  /**
   * We make the register value large enough to hold AVX-512.
   */
  static constexpr int MaxRegisterValueSize = 8;
  using DataType = ::LLVM::TDG::DataType;
  class RegisterValue : public std::array<uint64_t, MaxRegisterValueSize> {
  public:
    RegisterValue() : std::array<uint64_t, MaxRegisterValueSize>() {
      this->fill(0);
    }
    std::string print(const DataType &type) const;
    std::string print() const;
    uint8_t *uint8Ptr(int offset = 0) {
      assert(offset < 64);
      return reinterpret_cast<uint8_t *>(&this->front()) + offset;
    }
    const uint8_t *uint8Ptr(int offset = 0) const {
      assert(offset < 64);
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
  };
  static int translateToNumRegs(const DataType &type);
  static std::string printRegisterValue(const RegisterValue &value,
                                        const DataType &type);

  ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func);

  RegisterValue invoke(const std::vector<RegisterValue> &params,
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

  int getNumInstructions() const { return this->instructions.size(); }
  const std::vector<StaticInstPtr> &getStaticInsts() const {
    return this->instructions;
  }

private:
  ThreadContext *tc;
  const ::LLVM::TDG::ExecFuncInfo &func;
  // All the types are integer.
  bool isPureInteger;
  bool isSIMD = false;
  Cycles estimatedLatency;

  std::vector<StaticInstPtr> instructions;
  std::vector<PCState> pcs;

  void estimateLatency();
};
} // namespace X86ISA

std::ostream &operator<<(std::ostream &os,
                         const X86ISA::ExecFunc::RegisterValue &value);

#endif