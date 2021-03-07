#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_LSQ_CALLBACK_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_LSQ_CALLBACK_HH__

#include "stream_engine.hh"

/**
 * Callback structures for LSQ.
 * TODO: It seems these two classes can be merged into one.
 */
struct StreamLQCallback : public GemForgeLQCallback {
public:
  StreamElement *element;
  const FIFOEntryIdx FIFOIdx;
  /**
   * We construct the args here so that we can later call
   * areUsedStreamsReady(). And we need a copy of usedStreamIds as args only
   * saves a reference.
   */
  std::vector<uint64_t> usedStreamIds;
  StreamEngine::StreamUserArgs args;
  StreamLQCallback(StreamElement *_element, uint64_t _userSeqNum, Addr _userPC,
                   const std::vector<uint64_t> &_usedStreamIds)
      : element(_element), FIFOIdx(_element->FIFOIdx),
        usedStreamIds(_usedStreamIds),
        args(_userSeqNum, _userPC, usedStreamIds, false /* isStore */) {}
  bool getAddrSize(Addr &addr, uint32_t &size) const override;
  bool hasNonCoreDependent() const override;
  bool isIssued() const override;
  bool isValueReady() const override;
  void RAWMisspeculate() override;
  bool bypassAliasCheck() const override;
  std::ostream &format(std::ostream &os) const override {
    os << this->FIFOIdx;
    GemForgeLQCallback::format(os);
    return os;
  }
};

struct StreamSQCallback : public GemForgeSQCallback {
public:
  StreamElement *element;
  const FIFOEntryIdx FIFOIdx;
  /**
   * We construct the args here so that we can later call
   * areUsedStreamsReady(). And we need a copy of usedStreamIds as args only
   * saves a reference.
   */
  std::vector<uint64_t> usedStreamIds;
  StreamEngine::StreamUserArgs args;
  StreamSQCallback(StreamElement *_element, uint64_t _userSeqNum, Addr _userPC,
                   const std::vector<uint64_t> &_usedStreamIds);
  bool getAddrSize(Addr &addr, uint32_t &size) const override;
  bool hasNonCoreDependent() const override;
  bool isIssued() const override;
  bool isValueReady() const override;
  const uint8_t *getValue() const override;
  void RAWMisspeculate() override;
  bool bypassAliasCheck() const override;
  std::ostream &format(std::ostream &os) const override {
    os << this->FIFOIdx;
    GemForgeSQCallback::format(os);
    return os;
  }
};

struct StreamSQDeprecatedCallback : public GemForgeSQDeprecatedCallback {
public:
  StreamElement *element;
  StreamStoreInst *storeInst;
  StreamSQDeprecatedCallback(StreamElement *_element,
                             StreamStoreInst *_storeInst)
      : element(_element), storeInst(_storeInst) {}
  bool getAddrSize(Addr &addr, uint32_t &size) override;
  void writeback() override;
  bool isWritebacked() override;
  void writebacked() override;
};

#endif