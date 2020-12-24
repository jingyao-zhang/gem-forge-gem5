
#ifndef __GEM_FORGE_X86_EXEC_FUNC_HH__
#define __GEM_FORGE_X86_EXEC_FUNC_HH__

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif
#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"

#include <array>

namespace X86ISA {
class ExecFunc {
public:
  /**
   * We make the register value large enough to hold AVX-512.
   */
  static constexpr int MaxRegisterValueSize = 8;
  using RegisterValue = std::array<uint64_t, MaxRegisterValueSize>;
  using DataType = ::LLVM::TDG::DataType;
  static int translateToNumRegs(const DataType &type);
  static std::string printRegisterValue(const RegisterValue &value,
                                        const DataType &type);

  ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func);

  RegisterValue invoke(const std::vector<RegisterValue> &params);

  /**
   * A special interface for address generation. Mostly used for indirect
   * streams.
   */
  Addr invoke(const std::vector<Addr> &params);

private:
  ThreadContext *tc;
  const ::LLVM::TDG::ExecFuncInfo &func;
  // All the types are integer.
  bool isPureInteger;

  std::vector<StaticInstPtr> instructions;
  std::vector<PCState> pcs;
};
} // namespace X86ISA

#endif