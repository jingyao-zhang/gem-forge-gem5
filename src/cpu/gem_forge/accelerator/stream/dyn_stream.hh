#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "cache/DynamicStreamId.hh"

#include "addr_gen_callback.hh"

#include <memory>
#include <vector>

struct DynamicStreamParam {
  union {
    uint64_t invariant;
    uint64_t baseStreamId;
  } param;
  bool isInvariant;
};

/**
 * Holds some information of a dynamic instance of a stream,
 * e.g. callback to generate addresses.
 */
struct DynamicStream {

  const DynamicStreamId dynamicStreamId;
  const uint64_t configSeqNum;

  // Whether the dynamic stream is offloaded to cache.
  bool offloadedToCache = false;

  // Whether the StreamConfig has executed (ready to go).
  bool configExecuted = false;

  // Params used to compute address.
  std::vector<DynamicStreamParam> formalParams;

  // Callback to generate the address.
  std::unique_ptr<AddrGenCallback> addrGenCallback;

  DynamicStream(const DynamicStreamId &_dynamicStreamId, uint64_t _configSeqNum)
      : dynamicStreamId(_dynamicStreamId), configSeqNum(_configSeqNum) {}
};

#endif