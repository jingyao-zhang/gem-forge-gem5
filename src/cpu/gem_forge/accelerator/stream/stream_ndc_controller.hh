#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_NDC_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_NDC_CONTROLLER_HH__

#include "stream_engine.hh"
#include "stream_ndc_packet.hh"

/**
 * Helper class to manager per element fine-grained near-data computing
 * for streams. This is mainly used as a baseline for near-stream computing.
 * Computations are offloaded for each element.
 */

class StreamNDCController {
public:
  StreamNDCController(StreamEngine *_se);

  using StreamList = std::list<Stream *>;
  using DynStreamList = std::list<DynStream *>;

  using StreamConfigArgs = StreamEngine::StreamConfigArgs;
  void offloadStreams(const StreamConfigArgs &args,
                      const ::LLVM::TDG::StreamRegion &region,
                      DynStreamList &dynStreams);

  bool canIssueNDCPacket(StreamElement *element);
  void issueNDCPacket(StreamElement *element);

private:
  StreamEngine *se;
};

#endif
