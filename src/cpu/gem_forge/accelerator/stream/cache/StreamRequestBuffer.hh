#ifndef __GEM_FORGE_STREAM_REQUEST_BUFFER_HH__
#define __GEM_FORGE_STREAM_REQUEST_BUFFER_HH__

#include "DynamicStreamId.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

// Generated by Ruby.
#include "mem/ruby/protocol/RequestMsg.hh"

#include <unordered_map>

/**
 * This class buffers issued stream requests. It will try to
 * limit the maximum number of inqueue requests per stream and
 * buffers overflowed requests. This is mainly used for fairness
 * and avoid unbalanced case.
 * NOTE: This is currently coupled with RequestMsg type.
 */

class StreamRequestBuffer {
public:
  using RequestPtr = std::shared_ptr<RequestMsg>;

  StreamRequestBuffer(AbstractStreamAwareController *_controller,
                      MessageBuffer *_outBuffer, Cycles _latency,
                      int _maxInqueueRequestsPerStream);
  void pushRequest(RequestPtr request);

  int curRemoteBank() const { return this->controller->getMachineID().num; }
  const char *curRemoteMachineType() const {
    return this->controller->getMachineTypeString();
  }

  int getTotalBufferedRequests() const { return this->totalBufferedRequests; }

private:
  AbstractStreamAwareController *controller;
  MessageBuffer *outBuffer;
  const Cycles latency;
  const int maxInqueueRequestsPerStream;
  int totalBufferedRequests = 0;

  struct InqueueStreamState {
    int inqueueRequests = 0;
    std::list<RequestPtr> buffered;
  };
  using InqueueStreamMapT =
      std::unordered_map<DynamicStreamId, InqueueStreamState,
                         DynamicStreamIdHasher>;
  using InqueueStreamMapIter = InqueueStreamMapT::iterator;
  InqueueStreamMapT inqueueStreamMap;

  void dequeue(MsgPtr msg);

  /**
   * Get or initialize the inqueue stream map entry.
   */
  InqueueStreamMapIter getOrInitInqueueState(const RequestPtr &request);

  /**
   * Enqueue the request into the OutBuffer.
   */
  void enqueue(const RequestPtr &request, int &inqueueRequests);
};

#endif