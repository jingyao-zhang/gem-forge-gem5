#ifndef __CPU_GEM_FORGE_MLC_STREAM_NDC_CONTROLLER_HH__
#define __CPU_GEM_FORGE_MLC_STREAM_NDC_CONTROLLER_HH__

#include "MLCStreamEngine.hh"

#include "cpu/gem_forge/accelerator/stream/stream_ndc_packet.hh"

#include <unordered_map>

class MLCStreamNDCController {
public:
  MLCStreamNDCController(MLCStreamEngine *_mlcSE);

  void receiveStreamNDCRequest(PacketPtr pkt);
  void receiveStreamNDCResponse(const ResponseMsg &msg);

private:
  MLCStreamEngine *mlcSE;

  using NDCPacketMapT =
      std::unordered_map<FIFOEntryIdx, StreamNDCPacketPtr, FIFOEntryIdxHasher>;
  using NDCPacketMapIter = NDCPacketMapT::iterator;
  NDCPacketMapT ndcPacketMap;

  void addNDCPacket(StreamNDCPacketPtr &ndc);
  NDCPacketMapIter getNDCPacket(const DynamicStreamSliceId &sliceId);
};

#endif