#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_STREAM_ENGINE_H__

/**
 * Stream engine at MLC cache level, which is private and
 * connects to a lower shared cache.
 *
 * This is where it receives the offloaded streams' data.
 */

#include "mem/packet.hh"

class AbstractStreamAwareController;
class StreamMemAccess;

class MLCStreamEngine {
public:
  MLCStreamEngine(AbstractStreamAwareController *_controller);
  ~MLCStreamEngine();

  void receiveMiss(PacketPtr pkt);
  int getCacheLevel(PacketPtr pkt);
  void serveMiss(PacketPtr pkt);

private:
  AbstractStreamAwareController *controller;

  bool isStreamPacket(PacketPtr pkt) const {
    return this->getStreamMemAccessFromPacket(pkt) != nullptr;
  }
  StreamMemAccess *getStreamMemAccessFromPacket(PacketPtr pkt) const;
};

#endif