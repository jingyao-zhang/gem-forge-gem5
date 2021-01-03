#ifndef __GEM_FORGE_ISA_HANDLER_HH__
#define __GEM_FORGE_ISA_HANDLER_HH__

/**
 * A place to implement the actual instruction functionality.
 */
#include "cpu/gem_forge/gem_forge_dyn_inst_info.hh"
#include "stream/isa_stream_engine.hh"

#include <unordered_map>

class GemForgeCPUDelegator;

class GemForgeISAHandler {
public:
  GemForgeISAHandler(GemForgeCPUDelegator *_cpuDelegaor)
      : cpuDelegator(_cpuDelegaor), se(_cpuDelegaor) {}

  void takeOverBy(GemForgeCPUDelegator *newDelegator);

  bool shouldCountInPipeline(const GemForgeDynInstInfo &dynInfo);
  bool canDispatch(const GemForgeDynInstInfo &dynInfo);

  /**
   * Dispatch the instruction, and generate extra LQ callbacks.
   * ! Note: It is the core's responsibility to ensure its LSQ has enough space.
   */
  void dispatch(const GemForgeDynInstInfo &dynInfo,
                GemForgeLSQCallbackList &extraLSQCallbacks);
  bool canExecute(const GemForgeDynInstInfo &dynInfo);
  void execute(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  bool canCommit(const GemForgeDynInstInfo &dynInfo);
  void commit(const GemForgeDynInstInfo &dynInfo);
  void rewind(const GemForgeDynInstInfo &dynInfo);

  void storeTo(Addr vaddr, int size);

private:
  GemForgeCPUDelegator *cpuDelegator;

  enum GemForgeStaticInstOpE {
    NORMAL, // Normal instructions.
    STREAM_CONFIG,
    STREAM_END,
    STREAM_INPUT,
    STREAM_READY,
    STREAM_LOAD,
    STREAM_FLOAD,
    STREAM_STEP,
    STREAM_STORE,
  };
  struct GemForgeStaticInstInfo {
    GemForgeStaticInstOpE op;
  };
  mutable std::unordered_map<Addr, GemForgeStaticInstInfo>
      cachedStaticMicroInstInfo;
  mutable std::unordered_map<Addr, GemForgeStaticInstInfo>
      cachedStaticMacroInstInfo;

  GemForgeStaticInstInfo &getStaticInstInfo(const GemForgeDynInstInfo &dynInfo);

  ISAStreamEngine se;
};

#endif