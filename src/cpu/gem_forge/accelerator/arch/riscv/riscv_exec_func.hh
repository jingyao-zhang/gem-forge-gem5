#ifndef __GEM_FORGE_RISCV_EXEC_FUNC_HH__
#define __GEM_FORGE_RISCV_EXEC_FUNC_HH__

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif
#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "cpu/thread_context.hh"

namespace RiscvISA {
class ExecFunc {
public:
  ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func);

  uint64_t invoke(const std::vector<uint64_t> &params);

private:
  ThreadContext *tc;
  const ::LLVM::TDG::ExecFuncInfo &func;

  TheISA::Decoder *decoder;
  Addr funcStartVAddr;
  std::vector<StaticInstPtr> instructions;
};

} // namespace RiscvISA

#endif