#ifndef __GEM_FORGE_STREAM_FUNC_ADDRESS_ENGINE_HH__
#define __GEM_FORGE_STREAM_FUNC_ADDRESS_ENGINE_HH__

#if THE_ISA == RISCV_ISA
#include "riscv/riscv_func_addr_callback.hh"
#elif THE_ISA == X86_ISA
#include "x86/x86_func_addr_callback.hh"
#else
#endif

#endif