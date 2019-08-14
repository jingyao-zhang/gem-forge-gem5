#ifndef __CPU_TDG_ACCELERATOR_STREAM_L0_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_L0_STREAM_ENGINE_H__

/**
 * Stream engine at L0 cache level, which is private and
 * only connects to a lower/larger private cache.
 *
 * This is like a place-holder class for future implementation.
 * So far the only job is determine if the stream is bypassed
 * by this cache level.
 */

#include "mem/packet.hh"

#include "cpu/gem_forge/accelerator/stream/cache/DynamicStreamSliceId.hh"

#include "CacheStreamConfigureData.hh"

#include <unordered_set>

class AbstractStreamAwareController;
class StreamMemAccess;

class L0StreamEngine {
public:
  L0StreamEngine(AbstractStreamAwareController *_controller);
  ~L0StreamEngine();

  bool isStreamAccess(PacketPtr pkt) const;
  void receiveStreamConfigure(PacketPtr pkt);
  bool shouldForward(PacketPtr pkt);
  bool shouldCache(PacketPtr pkt);
  void serveMiss(PacketPtr pkt);

  DynamicStreamSliceId getSliceId(PacketPtr pkt) const;  

private:
  AbstractStreamAwareController *controller;

  /**
   * Set of all offloaded streams.
   */
  std::unordered_set<DynamicStreamId, DynamicStreamIdHasher> offloadedStreams;

  StreamMemAccess *getStreamMemAccessFromPacket(PacketPtr pkt) const;
};

#endif