#ifndef __GEM_FORGE_ACCELERATOR_STREAM_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_HH__

#include "cache/CacheStreamConfigureData.hh"
#include "cpu/gem_forge/llvm_insts.hh"
#include "dyn_stream.hh"
#include "stream_element.hh"
#include "stream_statistic.hh"

#include "base/types.hh"
#include "mem/packet.hh"

#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "StreamMessage.pb.h"

#include <list>

class LLVMTraceCPU;
class GemForgeCPUDelegator;

class StreamEngine;
class StreamConfigInst;
class StreamEndInst;

/**
 * Holdes the aggregated stream state, across multiple dynamic streams.
 */
class Stream {
public:
  struct StreamArguments {
    LLVMTraceCPU *cpu;
    GemForgeCPUDelegator *cpuDelegator;
    StreamEngine *se;
    int maxSize;
    const ::LLVM::TDG::StreamRegion *streamRegion;
    uint64_t staticId;
    const char *name;
  };

  Stream(const StreamArguments &args);
  virtual ~Stream();

  /**
   * Stream initialization is divided into 2 phases:
   * 1. Create the basic unit -- as the place holder.
   * 2. Finalize it.
   *    a. For coalesced stream -- choose the prime logical stream.
   *    b. Find base streams.
   *    c. Find back base streams.
   * Notice that some information are not valid until finalized, e.g.
   * StreamName, StaticId.
   */
  virtual void finalize() = 0;
  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  void addBackBaseStream(Stream *backBaseStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);

  const std::string &getStreamName() const { return this->streamName; }
  virtual const std::string &getStreamType() const = 0;
  bool isMemStream() const;
  virtual uint32_t getLoopLevel() const = 0;
  virtual uint32_t getConfigLoopLevel() const = 0;
  virtual int32_t getElementSize() const = 0;
  virtual bool getFloatManual() const = 0;
  /**
   * Get coalesce base stream, 0 for invalid.
   */
  virtual uint64_t getCoalesceBaseStreamId() const { return 0; }
  virtual int32_t getCoalesceOffset() const { return 0; }

  virtual void prepareNewElement(StreamElement *element) {
    // Deprecated.
    panic("prepareNewElement is deprecated.");
  }

  /**
   * Simple bookkeeping information for the stream engine.
   */
  bool configured;
  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  size_t stepSize;
  size_t maxSize;
  FIFOEntryIdx FIFOIdx;
  int lateFetchCount;

  const ::LLVM::TDG::StreamRegion *streamRegion;
  uint64_t staticId;
  std::string streamName;

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
  std::unordered_set<Stream *> backDependentStreams;

  /**
   * Per stream statistics.
   */
  StreamStatistic statistic;
  void dumpStreamStats(std::ostream &os) const;

  void tick();

  virtual uint64_t getTrueFootprint() const = 0;
  virtual uint64_t getFootprint(unsigned cacheBlockSize) const = 0;
  virtual bool isContinuous() const = 0;

  LLVMTraceCPU *getCPU() { return this->cpu; }
  GemForgeCPUDelegator *getCPUDelegator() { return this->cpuDelegator; }

  virtual void configure(uint64_t seqNum, ThreadContext *tc) = 0;

  void dispatchStreamConfig(uint64_t seqNum, ThreadContext *tc);
  void executeStreamConfig(uint64_t seqNum,
                           const std::vector<uint64_t> *inputVec);
  void rewindStreamConfig(uint64_t seqNum);
  bool isStreamConfigureExecuted(uint64_t seqNum);

  void dispatchStreamEnd(uint64_t seqNum);
  void rewindStreamEnd(uint64_t seqNum);
  void commitStreamEnd(uint64_t seqNum);

  /***********************************************************************
   * API to manage the elements of this stream.
   ***********************************************************************/
  /**
   * Get the total number of allocated elements among all dynamic streams.
   */
  int getAllocSize() const { return this->allocSize; }

  /**
   * Add one element to the last dynamic stream.
   */
  void allocateElement(StreamElement *newElement);
  /**
   * Remove one stepped element from the first dynamic stream.
   */
  StreamElement *releaseElementStepped();
  /**
   * Remove one unstepped element from the first dynamic stream.
   */
  StreamElement *releaseElementUnstepped();
  /**
   * Check if the last dynamic stream can be stepped.
   */
  bool canStep() const { return this->allocSize - this->stepSize >= 2; }
  /**
   * Step one element of the last dynamic stream.
   */
  StreamElement *stepElement();
  /**
   * Unstep one element.
   */
  StreamElement *unstepElement();
  /**
   * Get the first unstepped element of the last dynamic stream.
   */
  StreamElement *getFirstUnsteppedElement();
  /**
   * Get previous element in the chain of the stream.
   * Notice that it may return the (dummy) element->stream->tail if this is
   * the first element for that stream.
   */
  StreamElement *getPrevElement(StreamElement *element);

  /**
   * Called by executeStreamConfig() to allow derived class to set up the
   * AddrGenCallback in DynamicStream.
   */
  virtual void setupAddrGen(DynamicStream &dynStream,
                            const std::vector<uint64_t> *inputVec) = 0;

  /**
   * For debug.
   */
  void dump() const;

  /**
   * ! Sean: StreamAwareCache
   * Allocate the CacheStreamConfigureData.
   */
  CacheStreamConfigureData *allocateCacheConfigureData(uint64_t configSeqNum,
                                                       bool isIndirect = false);

  /**
   * Helper function used in StreamAwareCache.
   */
  bool isDirectLoadStream() const;
  bool isDirectMemStream() const;
  virtual bool isPointerChaseLoadStream() const { return false; }
  virtual uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const = 0;

  std::deque<DynamicStream> dynamicStreams;
  DynamicStream &getDynamicStream(uint64_t seqNum);
  DynamicStream &getLastDynamicStream() {
    assert(!this->dynamicStreams.empty() && "No dynamic stream.");
    return this->dynamicStreams.back();
  }

  LLVMTraceCPU *cpu;
  GemForgeCPUDelegator *cpuDelegator;
  StreamEngine *se;

protected:
  std::unordered_set<Stream *> baseStepStreams;
  std::unordered_set<Stream *> baseStepRootStreams;
  std::unordered_set<Stream *> dependentStepStreams;

  /**
   * Total allocated elements among all dynamic streams.
   */
  size_t allocSize;

  /**
   * Step the dependent streams in this order.
   */
  std::list<Stream *> stepStreamList;

  bool isStepRoot() const {
    const auto &type = this->getStreamType();
    return this->baseStepStreams.empty() && (type == "phi" || type == "store");
  }

  /**
   * Helper function to setup a linear addr func.
   */
  void setupLinearAddrFunc(DynamicStream &dynStream,
                           const std::vector<uint64_t> *inputVec,
                           const LLVM::TDG::StreamInfo &info);
  /**
   * Helper function to setup an func addr func.
   */
  void setupFuncAddrFunc(DynamicStream &dynStream,
                         const std::vector<uint64_t> *inputVec,
                         const LLVM::TDG::StreamInfo &info);
};

#endif