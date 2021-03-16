#ifndef __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_ELEMENT_HH__

#include "addr_gen_callback.hh"
#include "base/types.hh"
#include "cache/DynamicStreamSliceId.hh"
#include "cpu/gem_forge/gem_forge_packet_handler.hh"
#include "fifo_entry_idx.hh"

#include <unordered_map>
#include <unordered_set>

class Stream;
class DynamicStream;
class StreamEngine;
class StreamStoreInst;

/**
 * Represent the breakdown of one element according to cache block size.
 */
class StreamMemAccess;
struct CacheBlockBreakdownAccess {
  // Which cache block this access belongs to.
  uint64_t cacheBlockVAddr = 0;
  // The actual virtual address.
  uint64_t virtualAddr = 0;
  // The actual size.
  uint8_t size = 0;
  // The StreamMemAccess that's inorder to bring the data.
  StreamMemAccess *memAccess = nullptr;
  /**
   * State of the cache line.
   * ! Faulted is treated as a poison value and should be propagated
   * ! to any user.
   */
  enum StateE {
    None,
    Initialized,
    Faulted,
    Issued,
    PrevElement,
    Ready
  } state = None;
  static std::string stateToString(StateE state);
  void clear() {
    this->state = CacheBlockBreakdownAccess::StateE::None;
    this->cacheBlockVAddr = 0;
    this->virtualAddr = 0;
    this->size = 0;
    this->memAccess = nullptr;
  }
};
std::ostream &operator<<(std::ostream &os,
                         const CacheBlockBreakdownAccess::StateE &state);

class StreamElement;
/**
 * This is used as a handler to the response packet.
 * The stream aware cache also use this to find the stream the packet belongs
 * to.
 *
 * Update 2019.11.22 By Zhengrong.
 * Due to the complicate requirement to coalesce continuous elements
 * belong to the same stream, we extend both the StreamMemAccess and
 * StreamElement to reflect the many to many mapping.
 * A StreamMemAccess has a leading element, which allocates it, and
 * is used as the sliceId for stream floating.
 * It also contains a receiver list, which are elements who expect
 * its response. Notice that a receiver may deregister itself from
 * the list if it's flushed and reissued (due to misspeculation).
 *
 */
class StreamMemAccess final : public GemForgePacketHandler {
public:
  StreamMemAccess(Stream *_stream, StreamElement *_element,
                  Addr _cacheBlockAddr, Addr _vaddr, int _size,
                  int _additionalDelay = 0);
  virtual ~StreamMemAccess() {}
  void handlePacketResponse(GemForgeCPUDelegator *cpuDelegator,
                            PacketPtr packet) override;
  void issueToMemoryCallback(GemForgeCPUDelegator *cpuDelegator) override;
  void handlePacketResponse(PacketPtr packet);

  Stream *getStream() const { return this->stream; }

  const DynamicStreamId &getDynamicStreamId() const {
    return this->FIFOIdx.streamId;
  }
  /**
   * TODO: Return a reference.
   */
  DynamicStreamSliceId getSliceId() const { return this->sliceId; }

  void setAdditionalDelay(int additionalDelay) {
    this->additionalDelay = additionalDelay;
  }

  struct ResponseEvent : public Event {
  public:
    GemForgeCPUDelegator *cpuDelegator;
    StreamMemAccess *memAccess;
    PacketPtr pkt;
    std::string n;
    ResponseEvent(GemForgeCPUDelegator *_cpuDelegator,
                  StreamMemAccess *_memAccess, PacketPtr _pkt)
        : cpuDelegator(_cpuDelegator), memAccess(_memAccess), pkt(_pkt),
          n("StreamMemAccessResponseEvent") {
      this->setFlags(EventBase::AutoDelete);
    }
    void process() override {
      this->memAccess->handlePacketResponse(this->cpuDelegator, this->pkt);
    }

    const char *description() const { return "StreamMemAccessResponseEvent"; }

    const std::string name() const { return this->n; }
  };

  Stream *const stream;
  /**
   * Leading element.
   */
  StreamElement *const element;
  // Whether this is a reissue access in case element is released/flushed.
  const bool isReissue;
  // Make a copy of the FIFOIdx in case element is released.
  const FIFOEntryIdx FIFOIdx;

  Addr cacheBlockVAddr;
  Addr vaddr;
  int size;

  // The slice of the this memory request.
  DynamicStreamSliceId sliceId;

  /**
   * Stores the elements that expect the response from this access.
   */
  static constexpr int MAX_NUM_RECEIVERS = 64;
  std::array<std::pair<StreamElement *, bool>, MAX_NUM_RECEIVERS> receivers;
  int numReceivers = 0;
  void registerReceiver(StreamElement *element);
  void deregisterReceiver(StreamElement *element);

  /**
   * Additional delay we want to add after we get the response.
   */
  int additionalDelay;
};

struct StreamElement {
  using StaticId = DynamicStreamId::StaticId;
  struct BaseElement {
    StreamElement *element;
    FIFOEntryIdx idx;
    BaseElement(StreamElement *_element)
        : element(_element), idx(_element->FIFOIdx) {
      assert(this->element->stream && "This is element has not allocated.");
    }
    bool isValid() const { return this->element->FIFOIdx == this->idx; }
  };
  // TODO: AddrBaseElement should also be tracked with BaseElement.
  std::unordered_set<StreamElement *> addrBaseElements;
  std::vector<BaseElement> valueBaseElements;
  StreamElement *next;
  Stream *stream;
  DynamicStream *dynS;
  StreamEngine *se;
  FIFOEntryIdx FIFOIdx;
  int cacheBlockSize;
  /**
   * Whether this element's value managed in cache block level.
   * So far all memory streams are managed in cache block level.
   * TODO: Handle indirect stream where this is not true.
   */
  bool isCacheBlockedValue;
  /**
   * Whether the first user of this stream element has been dispatched.
   * This is used to determine the first user the of the stream element
   * and allocate entry in the load queue.
   * Note: UpdateStream has a StreamLoad and StreamStore. We track both.
   */
  uint64_t firstUserSeqNum;
  uint64_t firstStoreSeqNum;
  bool isFirstUserDispatched() const;
  bool isFirstStoreDispatched() const;
  bool isStepped = false;
  bool isAddrAliased = false;
  bool isValueReady = false;
  bool isCacheAcked = false;
  bool flushed = false;

  Cycles allocateCycle;
  Cycles addrReadyCycle;
  Cycles issueCycle;
  Cycles valueReadyCycle;

  // First time a user check if value is ready.
  mutable Cycles firstValueCheckCycle;
  mutable Cycles firstValueCheckByCoreCycle;

  /**
   * Small vector stores the cache blocks this element touched.
   */
  uint64_t addr = 0;
  uint64_t size = 0;
  static constexpr int MAX_CACHE_BLOCKS = 24;
  int cacheBlocks = 0;
  CacheBlockBreakdownAccess cacheBlockBreakdownAccesses[MAX_CACHE_BLOCKS];
  /**
   * Small vector stores all the data.
   * * The value should be indexed in cache line granularity.
   * * i.e. the first byte of value is the byte at
   * * cacheBlockBreakdownAccesses[0].cacheBlockVAddr.
   * * Please use setValue() and getValue() to interact with value so that this
   * * is always respected.
   * This design is a compromise with existing implementation of coalescing
   * continuous stream elements, which allows an element to hold a little bit of
   * more data in the last cache block beyond its size.
   */
  std::vector<uint8_t> value;
  void setValue(StreamElement *prevElement);
  void setValue(Addr vaddr, int size, const uint8_t *val);
  void getValue(Addr vaddr, int size, uint8_t *val) const;
  void getValueByStreamId(StaticId streamId, uint8_t *val, int valLen) const;
  const uint8_t *getValuePtrByStreamId(StaticId streamId) const;
  uint64_t mapVAddrToValueOffset(Addr vaddr, int size) const;
  uint64_t mapVAddrToBlockOffset(Addr vaddr, int size) const;
  // Some helper template.
  template <typename T> void setValue(Addr vaddr, T *val) {
    this->setValue(vaddr, sizeof(*val), reinterpret_cast<uint8_t *>(val));
  }
  template <typename T> void getValue(Addr vaddr, T *val) {
    this->getValue(vaddr, sizeof(*val), reinterpret_cast<uint8_t *>(val));
  }
  template <typename T> void getValueByStreamId(StaticId streamId, T *val) {
    this->getValueByStreamId(streamId, reinterpret_cast<uint8_t *>(val),
                             sizeof(T));
  }
  StreamValue getValueBaseByStreamId(StaticId id);
  bool isValueFaulted(Addr vaddr, int size) const;

  /**
   * Check if value is ready, will set FirstCheckCycle.
   */
  bool checkValueReady(bool checkedByCore) const;
  bool checkValueBaseElementsValueReady() const;
  bool scheduledComputation = false;

  // Store the infly writeback memory accesses.
  std::unordered_map<StreamStoreInst *, std::unordered_set<StreamMemAccess *>>
      inflyWritebackMemAccess;

  bool stored;

  StreamElement(StreamEngine *_se);

  /**
   * Return true if the DynamicStream has known total trip count and this is the
   * last element.
   */
  bool isLastElement() const;

  /**
   * Return whether if this stream element should issue request to cache.
   * This assumes that this is a memory stream.
   */
  bool shouldIssue() const;

  StreamMemAccess *
  allocateStreamMemAccess(const CacheBlockBreakdownAccess &cacheBlockBreakDown);
  void handlePacketResponse(StreamMemAccess *memAccess, PacketPtr pkt);
  void markAddrReady();
  void computeValue();
  void tryMarkValueReady();
  void markValueReady();

  void splitIntoCacheBlocks();

  void dump() const;

  void clear();
  // This is to flush the element to just allocated state.
  void flush(bool aliased);
  void clearCacheBlocks();
  void clearInflyMemAccesses();
  void clearScheduledComputation();

  Stream *getStream() const {
    assert(this->stream != nullptr && "Null stream in the element.");
    return this->stream;
  }

  bool isAddrReady() const { return this->addrReady; }
  bool isReqIssued() const { return this->reqIssued; }
  void setReqIssued();
  bool isPrefetchIssued() const { return this->prefetchIssued; }
  void setPrefetchIssued();

  /**
   * Check if the computation value is ready.
   * For UpdateStream, this is UpdateValue.
   * Otherwise, this is just normal value.
   */
  bool isComputeValueReady() const;
  /**
   * API to access the UpdateValue for UpdateStream.
   */
  bool isUpdateValueReady() const { return this->updateValueReady; }
  bool checkUpdateValueReady() const;
  void receiveComputeResult(const StreamValue &result);
  const uint8_t *getUpdateValuePtrByStreamId(StaticId streamId) const;

  /**
   * Check if we can withdraw this element.
   */

private:
  bool addrReady = false;
  bool reqIssued = false;
  /**
   * We still issue prefetches for Store/AtomicStream in the core (not floated).
   * These prefetches are not expecting responses.
   */
  bool prefetchIssued = false;

  /**
   * Extra value for UpdateStream.
   * Unlike other streams which have only one value,
   * UpdateStream has two values: one loaded from cache and the other one
   * computed for store.
   */
  bool updateValueReady = false;
  StreamValue updateValue;

  /**
   * Helper func to udpate our stats about when first check on value happened.
   */
  void updateFirstValueCheckCycle(bool checkedByCore) const;
};

#endif