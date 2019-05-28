#ifndef __CPU_TDG_ACCELERATOR_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_HH__

#include "cpu/gem_forge/llvm_insts.hh"
#include "stream_element.hh"

#include "base/types.hh"
#include "mem/packet.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/gem_forge/accelerator/stream/StreamMessage.pb.h"

#include <list>

class LLVMTraceCPU;

class StreamEngine;
class StreamConfigInst;
class StreamStepInst;
class StreamStoreInst;
class StreamEndInst;

class Stream {
public:
  struct StreamArguments {
    LLVMTraceCPU *cpu;
    StreamEngine *se;
    int maxSize;
    const ::LLVM::TDG::StreamRegion *streamRegion;
  };

  Stream(const StreamArguments &args);

  virtual ~Stream();

  virtual const std::string &getStreamName() const = 0;
  virtual const std::string &getStreamType() const = 0;
  bool isMemStream() const;
  virtual uint32_t getLoopLevel() const = 0;
  virtual uint32_t getConfigLoopLevel() const = 0;
  virtual int32_t getElementSize() const = 0;

  virtual void prepareNewElement(StreamElement *element) = 0;

  /**
   * Simple bookkeeping information for the stream engine.
   */
  bool configured;
  StreamElement *head;
  StreamElement *stepped;
  StreamElement *tail;
  size_t allocSize;
  size_t stepSize;
  size_t maxSize;
  FIFOEntryIdx FIFOIdx;
  int lateFetchCount;

  const ::LLVM::TDG::StreamRegion *streamRegion;

  /**
   * Step root stream, three possible cases:
   * 1. this: I am the step root.
   * 2. other: I am controlled by other step stream.
   * 3. nullptr: I am a constant stream.
   */
  Stream *stepRootStream;
  std::unordered_set<Stream *> baseStreams;
  std::unordered_set<Stream *> dependentStreams;
  /**
   * Back edge dependence on previous iteration.
   */
  std::unordered_set<Stream *> backBaseStreams;

  /**
   * Per stream statistics.
   */
  size_t numConfigured;
  size_t numAllocated;
  size_t numFetched;
  size_t numStepped;
  size_t numUsed;

  void dumpStreamStats(std::ostream &os) const;

  void tick();

  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  virtual void initializeBackBaseStreams() = 0;
  void addBackBaseStream(Stream *backBaseStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);

  uint64_t getFirstConfigSeqNum() const { return this->firstConfigSeqNum; }
  bool isBeforeFirstConfigInst(uint64_t seqNum) const {
    if (this->firstConfigSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
    return seqNum < this->firstConfigSeqNum;
  }

  virtual uint64_t getTrueFootprint() const = 0;
  virtual uint64_t getFootprint(unsigned cacheBlockSize) const = 0;
  virtual bool isContinuous() const = 0;

  LLVMTraceCPU *getCPU() { return this->cpu; }

  virtual void configure(StreamConfigInst *inst) = 0;

  const std::unordered_map<uint64_t, int> &getAliveCacheBlocks() const {
    return this->aliveCacheBlocks;
  }

  bool isConfigured() const {
    if (this->configSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return false;
    }
    if (this->endSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
    return this->configSeqNum > this->endSeqNum;
  }

  /**
   * This is used as a handler to the response packet.
   * The stream aware cache also use this to find the stream the packet belongs
   * to.
   */
  class StreamMemAccess final : public TDGPacketHandler {
  public:
    StreamMemAccess(Stream *_stream, const FIFOEntryIdx _entryId,
                    int _additionalDelay = 0)
        : stream(_stream), entryId(_entryId),
          additionalDelay(_additionalDelay) {}
    virtual ~StreamMemAccess() {}
    void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;
    void handlePacketResponse(PacketPtr packet);

    Stream *getStream() const { return this->stream; }

    void setAdditionalDelay(int additionalDelay) {
      this->additionalDelay = additionalDelay;
    }

    struct ResponseEvent : public Event {
    public:
      LLVMTraceCPU *cpu;
      Stream::StreamMemAccess *memAccess;
      PacketPtr pkt;
      std::string n;
      ResponseEvent(LLVMTraceCPU *_cpu, Stream::StreamMemAccess *_memAccess,
                    PacketPtr _pkt)
          : cpu(_cpu), memAccess(_memAccess), pkt(_pkt),
            n("StreamMemAccessResponseEvent") {}
      void process() override {
        this->memAccess->handlePacketResponse(this->cpu, this->pkt);
      }

      const char *description() const { return "StreamMemAccessResponseEvent"; }

      const std::string name() const { return this->n; }
    };

  private:
    Stream *stream;
    FIFOEntryIdx entryId;
    /**
     * Additional delay we want to add after we get the response.
     */
    int additionalDelay;
  };

protected:
  LLVMTraceCPU *cpu;
  StreamEngine *se;

  std::unordered_set<Stream *> baseStepStreams;
  std::unordered_set<Stream *> baseStepRootStreams;
  std::unordered_set<Stream *> dependentStepStreams;

  StreamElement nilTail;

  /**
   * Step the dependent streams in this order.
   */
  std::list<Stream *> stepStreamList;

  bool isStepRoot() const {
    const auto &type = this->getStreamType();
    return this->baseStepStreams.empty() && (type == "phi" || type == "store");
  }

  uint64_t firstConfigSeqNum;
  uint64_t configSeqNum;
  uint64_t endSeqNum;

  /**
   * Dummy stored data used for store stream.
   * For simplicity, we just allocate one cache block here and let the packet
   * size tailor it as needed, as maximum size of a packet is a cache block.
   */
  uint8_t *storedData;

  std::unordered_set<StreamMemAccess *> memAccesses;

  mutable std::unordered_map<uint64_t, int> aliveCacheBlocks;
  void addAliveCacheBlock(uint64_t addr) const;
  void removeAliveCacheBlock(uint64_t addr) const;
  bool isCacheBlockAlive(uint64_t addr) const;
  uint64_t getCacheBlockAddr(uint64_t addr) const;

  // void updateRunAHeadLength(size_t newRunAHeadLength);

  virtual void handlePacketResponse(const FIFOEntryIdx &entryId,
                                    PacketPtr packet,
                                    StreamMemAccess *memAccess) = 0;
  // /**
  //  * For throttler.
  //  * TODO: extract to another class.
  //  */
  // void throttleLate();

  /**
   * For debug.
   */
  virtual void dump() const = 0;
};

#endif