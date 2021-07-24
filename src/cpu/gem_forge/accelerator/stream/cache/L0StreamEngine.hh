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

#include <unordered_map>

class AbstractStreamAwareController;
class StreamMemAccess;

/**
 * Hold the information of a configured L0 stream.
 * So far this is very simple, only the root dynamic stream id.
 */
class L0DynamicStream {
public:
  L0DynamicStream(const DynamicStreamId &_rootDynamicStreamId,
                  CacheStreamConfigureDataPtr _configData)
      : dynamicStreamId(_configData->dynamicId),
        rootDynamicStreamId(_rootDynamicStreamId),
        isOneIterationBehind(_configData->isOneIterationBehind),
        isPseudoOffload(_configData->isPseudoOffload),
        firstFloatElemIdx(_configData->firstFloatElementIdx) {}

  const DynamicStreamId &getDynamicStreamId() const {
    return this->dynamicStreamId;
  }
  const DynamicStreamId &getRootDynamicStreamId() const {
    return this->rootDynamicStreamId;
  }

  bool getIsOneIterationBehind() const { return this->isOneIterationBehind; }
  bool getIsPseudoOffload() const { return this->isPseudoOffload; }
  uint64_t getFirstFloatElemIdx() const { return this->firstFloatElemIdx; }

private:
  const DynamicStreamId dynamicStreamId;
  const DynamicStreamId rootDynamicStreamId;
  bool isOneIterationBehind;
  bool isPseudoOffload;
  uint64_t firstFloatElemIdx;
};

class L0StreamEngine {
public:
  L0StreamEngine(AbstractStreamAwareController *_controller);
  ~L0StreamEngine();

  bool isStreamAccess(PacketPtr pkt) const;
  void receiveStreamConfigure(PacketPtr pkt);
  void receiveStreamEnd(PacketPtr pkt);
  bool shouldForward(PacketPtr pkt);
  bool shouldCache(PacketPtr pkt);
  bool mustServedByMLCSE(PacketPtr pkt);

  DynamicStreamSliceId getSliceId(PacketPtr pkt) const;

private:
  AbstractStreamAwareController *controller;

  /**
   * Set of all offloaded streams, along with their root dynamic stream id.
   */
  std::unordered_map<const DynamicStreamId, L0DynamicStream *,
                     DynamicStreamIdHasher>
      offloadedStreams;

  StreamMemAccess *getStreamMemAccessFromPacket(PacketPtr pkt) const;
};

#endif