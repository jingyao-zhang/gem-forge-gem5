#ifndef __RISCV_GEM_FORGE_DYN_INST_INFO_HH__
#define __RISCV_GEM_FORGE_DYN_INST_INFO_HH__

#include "cpu/static_inst.hh"
#include "config/the_isa.hh"

#include "arch/x86/pcstate.hh"

namespace gem5 {

struct GemForgeDynInstInfo {
  const uint64_t seqNum;
  const PCStateBase &pc;
  StaticInst *staticInst;
  ThreadContext *tc;
  GemForgeDynInstInfo(uint64_t _seqNum, const PCStateBase &_pc,
                      StaticInst *_staticInst, ThreadContext *_tc)
      : seqNum(_seqNum), pc(_pc), staticInst(_staticInst), tc(_tc) {}
};
}

#endif