#ifndef __CPU_TDG_ACCELERATOR_STREAM_TRANSLATION_BUFFER_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_TRANSLATION_BUFFER_HH__

#include "arch/generic/tlb.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include <functional>
#include <unordered_set>

/**
 * Represent a simple translation buffer for each stream.
 */

template <typename T> class StreamTranslationBuffer {
public:
  using TranslationDoneCallback =
      std::function<void(PacketPtr, ThreadContext *, T)>;

  StreamTranslationBuffer(BaseTLB *_tlb, TranslationDoneCallback _doneCallback,
                          bool _accessLastLevelTLBOnly)
      : tlb(_tlb), doneCallback(_doneCallback),
        accessLastLevelTLBOnly(_accessLastLevelTLBOnly) {}

  void addTranslation(PacketPtr pkt, ThreadContext *tc, T data) {
    auto translation = new StreamTranslation(pkt, tc, data, this);
    this->inflyTranslationSet.insert(translation);

    // Start translation.
    this->startTranslation(translation);
  }

private:
  BaseTLB *tlb;
  TranslationDoneCallback doneCallback;
  /**
   * Whether we only go to last level TLB.
   */
  bool accessLastLevelTLBOnly;
  struct StreamTranslation;

  void startTranslation(StreamTranslation *translation) {
    BaseTLB::Mode mode =
        translation->pkt->isRead() ? BaseTLB::Mode::Read : BaseTLB::Mode::Write;
    if (this->accessLastLevelTLBOnly) {
      this->tlb->translateTimingAtLastLevel(translation->pkt->req,
                                            translation->tc, translation, mode);
    } else {
      this->tlb->translateTiming(translation->pkt->req, translation->tc,
                                 translation, mode);
    }
  }
  void finishTranslation(StreamTranslation *translation) {
    assert(this->inflyTranslationSet.count(translation));
    auto pkt = translation->pkt;
    auto tc = translation->tc;
    auto data = translation->data;
    this->doneCallback(pkt, tc, data);
    this->inflyTranslationSet.erase(translation);
    delete translation;
  }

  struct StreamTranslation : public BaseTLB::Translation {
    PacketPtr pkt;
    ThreadContext *tc;
    T data;
    StreamTranslationBuffer *buffer;
    StreamTranslation(PacketPtr _pkt, ThreadContext *_tc, T _data,
                      StreamTranslationBuffer *_buffer)
        : pkt(_pkt), tc(_tc), data(_data), buffer(_buffer) {}

    /**
     * Implement translation interface.
     */
    void markDelayed() override {
      // No need to do anything.
    }

    void finish(const Fault &fault, const RequestPtr &req, ThreadContext *tc,
                BaseTLB::Mode mode) override {
      assert(fault == NoFault && "Fault for StreamTranslation.");
      this->buffer->finishTranslation(this);
    }

    bool squashed() const override {
      // So far we do not support squashing.
      return false;
    }
  };

  std::unordered_set<StreamTranslation *> inflyTranslationSet;
};

#endif