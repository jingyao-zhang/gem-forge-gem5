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

class AbstractStreamAwareController;
class StreamMemAccess;

class L0StreamEngine {
public:
  L0StreamEngine(AbstractStreamAwareController *_controller);
  ~L0StreamEngine();

  void receiveMiss(PacketPtr pkt);
  void serveMiss(PacketPtr pkt);
  int getCacheLevel(PacketPtr pkt);

private:
  AbstractStreamAwareController *controller;

  bool isStreamPacket(PacketPtr pkt) const {
    return this->getStreamMemAccessFromPacket(pkt) != nullptr;
  }
  StreamMemAccess *getStreamMemAccessFromPacket(PacketPtr pkt) const;
};

#endif