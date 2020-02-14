#ifndef __GEM_FORGE_EXEC_FUNC_HH__
#define __GEM_FORGE_EXEC_FUNC_HH__

#include "config/the_isa.hh"

#if THE_ISA == RISCV_ISA
#include "riscv/riscv_exec_func.hh"
#elif THE_ISA == X86_ISA
#include "x86/x86_exec_func.hh"
#else
#endif

#endif