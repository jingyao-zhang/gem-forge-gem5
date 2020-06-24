#ifndef __GEM_FORGE_ISA_STREAM_ENGINE_H__
#define __GEM_FORGE_ISA_STREAM_ENGINE_H__

/**
 * An interface between the ssp instructions in a ISA and the real stream
 * engine. May get rid of this later when we have better code base in the real
 * stream engine.
 */

#include "cpu/gem_forge/gem_forge_dyn_inst_info.hh"
#include "cpu/gem_forge/gem_forge_lsq_callback.hh"

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include <array>
#include <unordered_map>

class StreamEngine;
class GemForgeCPUDelegator;

class ISAStreamEngine {
public:
  ISAStreamEngine(GemForgeCPUDelegator *_cpuDelegator)
      : cpuDelegator(_cpuDelegator) {
    for (auto i = 0; i < MaxNumRegionStreams; ++i) {
      this->regionStreamIdTable.at(i) = InvalidStreamId;
    }
  }

#define DeclareStreamInstHandler(Inst)                                         \
  bool canDispatchStream##Inst(const GemForgeDynInstInfo &dynInfo);            \
  void dispatchStream##Inst(const GemForgeDynInstInfo &dynInfo,                \
                            GemForgeLQCallbackList &extraLQCallbacks);         \
  bool canExecuteStream##Inst(const GemForgeDynInstInfo &dynInfo);             \
  void executeStream##Inst(const GemForgeDynInstInfo &dynInfo,                 \
                           ExecContext &xc);                                   \
  void commitStream##Inst(const GemForgeDynInstInfo &dynInfo);                 \
  void rewindStream##Inst(const GemForgeDynInstInfo &dynInfo);

  DeclareStreamInstHandler(Config);
  DeclareStreamInstHandler(Input);
  DeclareStreamInstHandler(Ready);
  DeclareStreamInstHandler(End);
  DeclareStreamInstHandler(Step);
  DeclareStreamInstHandler(Load);

#undef DeclareStreamInstHandler

  void storeTo(Addr vaddr, int size);

private:
  ::GemForgeCPUDelegator *cpuDelegator;

  ::StreamEngine *SE = nullptr;
  bool SEMemorized = false;
  ::StreamEngine *getStreamEngine();

  template <typename T> T extractImm(const StaticInst *staticInst) const;

  /**
   * Memorize the AllStreamRegions.
   */
  mutable std::unique_ptr<::LLVM::TDG::AllStreamRegions> allStreamRegions;
  const std::string &getRelativePath(int configIdx) const;

  /**
   * Memorize the StreamConfigureInfo.
   */
  mutable std::unordered_map<uint64_t, ::LLVM::TDG::StreamRegion>
      memorizedStreamRegionMap;
  const ::LLVM::TDG::StreamRegion &getStreamRegion(uint64_t configIdx) const;

  /**
   * Since the stream engine uses the full stream id,
   * we want to translate the regional stream id to it.
   * This is performed to reduce the complexity of the stream engine.
   */
  static constexpr int MaxNumRegionStreams = 128;
  std::array<uint64_t, MaxNumRegionStreams> regionStreamIdTable;
  static constexpr uint64_t InvalidStreamId = 0;
  void insertRegionStreamIds(const ::LLVM::TDG::StreamRegion &region);

  bool canSetRegionStreamIds(const ::LLVM::TDG::StreamRegion &region);
  bool canRemoveRegionStreamIds(const ::LLVM::TDG::StreamRegion &region);
  void removeRegionStreamIds(const ::LLVM::TDG::StreamRegion &region);
  uint64_t lookupRegionStreamId(int regionStreamId) const;
  bool isValidRegionStreamId(int regionStreamId) const;

  /**
   * StreamEngine is configured through a sequence of instructions:
   * ssp.stream.config
   * ssp.stream.input*
   * ssp.stream.ready
   * We hide this detail from the StreamEngine. When dispatched, all these
   * instructions will
   * be marked with the current DynStreamRegionInfo.
   * 1. When ssp.stream.ready dispatches, we call StreamEngine::canStreamConfig
   * and StreamEngine::dispatchStreamConfig.
   * 2. When all the instructions are executed, we inform the
   * StreamEngine::executeStreamConfig.
   * 3. When ssp.stream.ready commits, we call StreamEngine::commitStreamConfig.
   *
   * ssp.stream.config may set MustMisspeculated, which will be propagated to
   * later instructions.
   */
  struct DynStreamRegionInfo {
    using StreamInputValue = std::array<uint64_t, 8>;
    const std::string infoRelativePath;
    bool streamReadyDispatched = false;
    uint64_t streamReadySeqNum = 0;
    int numDispatchedInsts = 0;
    int numExecutedInsts = 0;
    bool mustBeMisspeculated = false;
    std::unordered_map<uint64_t, std::vector<StreamInputValue>> inputMap;
    // Mainly used for misspeculation recover.
    std::shared_ptr<DynStreamRegionInfo> prevRegion = nullptr;
    DynStreamRegionInfo(const std::string &_infoRelativePath,
                        std::shared_ptr<DynStreamRegionInfo> _prevRegion)
        : infoRelativePath(_infoRelativePath), prevRegion(_prevRegion) {}
  };

  /**
   * Store the current stream region info being used at dispatch stage.
   * We need a shared_ptr as it will be stored in DynStreamInstInfo and used
   * later in execution stage, etc.
   */
  std::shared_ptr<DynStreamRegionInfo> curStreamRegionInfo;

  /**
   * We need some extra information for each dynamic stream information.
   * ssp.stream.config
   * ssp.stream.input
   * ssp.stream.ready
   *   --> They require DynStreamRegionInfo.
   * ssp.stream.step
   *   --> Need the translated StreamId.
   */
  struct DynStreamConfigInstInfo {
    std::shared_ptr<DynStreamRegionInfo> dynStreamRegionInfo;
  };

  struct DynStreamInputInstInfo {
    uint64_t translatedStreamId = InvalidStreamId;
    int inputIdx = -1;
    bool executed = false;
  };

  struct DynStreamStepInstInfo {
    uint64_t translatedStreamId = InvalidStreamId;
  };

  struct DynStreamUserInstInfo {
    static constexpr int MaxUsedStreams = 2;
    std::array<uint64_t, MaxUsedStreams> translatedUsedStreamIds;
  };

  /**
   * We also remember the translated regionStreamId for every dynamic
   * instruction.
   */
  struct DynStreamInstInfo {
    /**
     * Maybe we can use a union to save the storage, but union is
     * painful to use when the member is not POD and I don't care.
     */
    DynStreamConfigInstInfo configInfo;
    DynStreamInputInstInfo inputInfo;
    DynStreamStepInstInfo stepInfo;
    DynStreamUserInstInfo userInfo;
    /**
     * Sometimes it is for sure this instruction is misspeculated.
     */
    bool mustBeMisspeculated = false;
  };
  std::unordered_map<uint64_t, DynStreamInstInfo> seqNumToDynInfoMap;

  DynStreamInstInfo &createDynStreamInstInfo(uint64_t seqNum);
  DynStreamInstInfo &getOrCreateDynStreamInstInfo(uint64_t seqNum);

  /**
   * Mark one stream config inst executed.
   * If all executed, will call StreamEngine::executeStreamConfig.
   */
  void increamentStreamRegionInfoNumExecutedInsts(
      DynStreamRegionInfo &dynStreamRegionInfo);
};

#endif