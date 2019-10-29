#include "x86_func_addr_callback.hh"

#define FUNC_ADDR_DPRINTF(format, args...)                                     \
  DPRINTF(FuncAddrCallback, "[%s]: " format, this->func.name().c_str(), ##args)

namespace {} // namespace

namespace X86ISA {

FuncAddrGenCallback::FuncAddrGenCallback(ThreadContext *_tc,
                                         const ::LLVM::TDG::AddrFuncInfo &_func)
    : tc(_tc), func(_func) {
  panic("FuncAddrGenCallbck is not implemented.\n");
}

uint64_t FuncAddrGenCallback::genAddr(uint64_t idx,
                                      const std::vector<uint64_t> &params) {
  panic("FuncAddrGenCallbck is not implemented.\n");
  return 0;
}
} // namespace X86ISA