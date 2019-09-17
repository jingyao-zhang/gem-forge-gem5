#ifndef __GEM_FORGE_RISCV_STREAM_ENGINE_H__
#define __GEM_FORGE_RISCV_STREAM_ENGINE_H__

/**
 * An interface between the riscv ssp instructions and the real stream engine.
 * May get rid of this later when we have better code base in the real stream
 * engine.
 */

#include "cpu/static_inst.hh"

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include <array>
#include <unordered_map>

class StreamEngine;

namespace RiscvISA {

// TODO: Move these into separate header.
struct GemForgeDynInstInfo {
  uint64_t seqNum;
  StaticInst *staticInst;
  GemForgeDynInstInfo(uint64_t _seqNum, StaticInst *_staticInst)
      : seqNum(_seqNum), staticInst(_staticInst) {}
};

class RISCVStreamEngine {
public:
#define DeclareStreamInstHandler(Inst)                                         \
  bool canDispatchStream##Inst(const GemForgeDynInstInfo &dynInfo,             \
                               ExecContext &xc);                               \
  void dispatchStream##Inst(const GemForgeDynInstInfo &dynInfo,                \
                            ExecContext &xc);                                  \
  void executeStream##Inst(const GemForgeDynInstInfo &dynInfo,                 \
                           ExecContext &xc);                                   \
  void commitStream##Inst(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);

  DeclareStreamInstHandler(Config);
  DeclareStreamInstHandler(End);
  DeclareStreamInstHandler(Step);

#undef DeclareStreamInstHandler

private:
  ::StreamEngine *getStreamEngine(ExecContext &xc);

  template <typename T> T extractImm(const StaticInst *staticInst) const;

  /**
   * Memorize the StreamConfigureInfo.
   */
  mutable std::unordered_map<std::string, ::LLVM::TDG::StreamRegion>
      memorizedStreamRegionMap;
  const ::LLVM::TDG::StreamRegion &
  getStreamRegion(const std::string &path) const;

  /**
   * Since the stream engine uses the full stream id,
   * we want to translate the regional stream id to it.
   * This is performed to reduce the complexity of the stream engine.
   */
  std::vector<uint64_t> regionStreamIdTable;
  static constexpr uint64_t InvalidStreamId = 0;
  void insertRegionStreamIds(const ::LLVM::TDG::StreamRegion &region);
  void removeRegionStreamIds(const ::LLVM::TDG::StreamRegion &region);
  uint64_t lookupRegionStreamId(int regionStreamId);

  /**
   * We also remember the translated regionStreamId for every dynamic
   * instruction.
   */
  struct DynStreamInstInfo {
    static constexpr int MaxStreamIds = 1;
    std::array<uint64_t, MaxStreamIds> translatedStreamIds;
    DynStreamInstInfo() {
      for (auto &streamId : this->translatedStreamIds) {
        streamId = InvalidStreamId;
      }
    }
  };
  std::unordered_map<uint64_t, DynStreamInstInfo> seqNumToDynInfoMap;
};

} // namespace RiscvISA

#endif