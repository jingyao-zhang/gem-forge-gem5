#ifndef __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_DYN_STREAM_HH__

#include "fifo_entry_idx.hh"

#include "addr_gen_callback.hh"

#include <memory>
#include <vector>

class ThreadContext;

/**
 * Holds some information of a dynamic instance of a stream,
 * e.g. callback to generate addresses.
 */
struct DynamicStream {

  const DynamicStreamId dynamicStreamId;
  const uint64_t configSeqNum;
  ThreadContext *tc;
  const FIFOEntryIdx prevFIFOIdx;

  // Whether the dynamic stream is offloaded to cache.
  bool offloadedToCache = false;

  // Whether the StreamConfig has executed (ready to go).
  bool configExecuted = false;

  // Params used to compute address.
  DynamicStreamFormalParamV formalParams;

  // Callback to generate the address.
  AddrGenCallbackPtr addrGenCallback;

  DynamicStream(const DynamicStreamId &_dynamicStreamId, uint64_t _configSeqNum,
                ThreadContext *_tc, const FIFOEntryIdx &_prevFIFOIdx)
      : dynamicStreamId(_dynamicStreamId), configSeqNum(_configSeqNum), tc(_tc),
        prevFIFOIdx(_prevFIFOIdx) {}
};

#endif