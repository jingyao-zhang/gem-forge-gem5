#ifndef __RISCV_GEM_FORGE_ISA_HANDLER_HH__
#define __RISCV_GEM_FORGE_ISA_HANDLER_HH__

/**
 * A place to implement the actual instruction functionality.
 */

#include "cpu/static_inst.hh"

#include <unordered_map>

namespace RiscvISA {

struct GemForgeDynInstInfo {
  uint64_t seqNum;
  StaticInst *staticInst;
  GemForgeDynInstInfo(uint64_t _seqNum, StaticInst *_staticInst)
      : seqNum(_seqNum), staticInst(_staticInst) {}
};

class GemForgeISAHandler {
public:
  bool canDispatch(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  void dispatch(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  void execute(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  void commit(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);

private:
  enum GemForgeStaticInstOpE {
    NORMAL, // Normal instructions.
    STREAM_CONFIG,
    STREAM_END,
    STREAM_INPUT,
    STREAM_LOAD,
    STREAM_STEP,
  };
  struct GemForgeStaticInstInfo {
    GemForgeStaticInstOpE op;
  };
  mutable std::unordered_map<Addr, GemForgeStaticInstInfo> cachedStaticInstInfo;

  GemForgeStaticInstInfo &getStaticInstInfo(const TheISA::PCState &pcState,
                                            const GemForgeDynInstInfo &dynInfo);
};

} // namespace RiscvISA

#endif