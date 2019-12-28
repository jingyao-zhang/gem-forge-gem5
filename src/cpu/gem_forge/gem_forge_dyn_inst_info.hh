#ifndef __RISCV_GEM_FORGE_DYN_INST_INFO_HH__
#define __RISCV_GEM_FORGE_DYN_INST_INFO_HH__

#include "cpu/static_inst.hh"

struct GemForgeDynInstInfo {
  const uint64_t seqNum;
  const TheISA::PCState pc;
  StaticInst *staticInst;
  ThreadContext *tc;
  GemForgeDynInstInfo(uint64_t _seqNum, const TheISA::PCState &_pc,
                      StaticInst *_staticInst, ThreadContext *_tc)
      : seqNum(_seqNum), pc(_pc), staticInst(_staticInst), tc(_tc) {}
};

#endif