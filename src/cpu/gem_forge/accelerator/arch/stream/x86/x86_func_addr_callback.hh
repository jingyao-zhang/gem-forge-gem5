
#ifndef __GEM_FORGE_X86_STREAM_ADDRESS_ENGINE_HH__
#define __GEM_FORGE_X86_STREAM_ADDRESS_ENGINE_HH__

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif
#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"
#include "cpu/gem_forge/accelerator/stream/addr_gen_callback.hh"

#include "cpu/thread_context.hh"

namespace X86ISA {
class FuncAddrGenCallback : public AddrGenCallback {
public:
  FuncAddrGenCallback(ThreadContext *_tc,
                      const ::LLVM::TDG::AddrFuncInfo &_func);

  uint64_t genAddr(uint64_t idx, const std::vector<uint64_t> &params) override;

private:
  ThreadContext *tc;
  const ::LLVM::TDG::AddrFuncInfo &func;

//   TheISA::Decoder *decoder;
//   Addr funcStartVAddr;
//   std::list<StaticInstPtr> instructions;
};
} // namespace X86ISA

#endif