
#ifndef __GEM_FORGE_X86_EXEC_FUNC_HH__
#define __GEM_FORGE_X86_EXEC_FUNC_HH__

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif
#include "cpu/gem_forge/accelerator/arch/exec_func.hh"
#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"

namespace X86ISA {
class ExecFunc {
public:
  ExecFunc(ThreadContext *_tc, const ::LLVM::TDG::ExecFuncInfo &_func);

  uint64_t invoke(const std::vector<uint64_t> &params);

private:
  ThreadContext *tc;
  const ::LLVM::TDG::ExecFuncInfo &func;

  std::vector<StaticInstPtr> instructions;
  std::vector<PCState> pcs;
};
} // namespace X86ISA

#endif