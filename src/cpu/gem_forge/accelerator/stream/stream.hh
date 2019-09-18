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
 * Holdes the aggregated stream state, across multiple dynamic stream.
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

  const std::string &getStreamName() const { return this->streamName; }
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
  /**
   * Head is the newest element.
   * Tail is the dummy node before the oldest element.
   */
  StreamElement *head;
  StreamElement *stepped;
  StreamElement *tail;
  size_t allocSize;
  size_t stepSize;
  size_t maxSize;
  FIFOEntryIdx FIFOIdx;
  int lateFetchCount;

  const ::LLVM::TDG::StreamRegion *streamRegion;
  const uint64_t staticId;
  const std::string streamName;

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

  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  virtual void initializeBackBaseStreams() = 0;
  void addBackBaseStream(Stream *backBaseStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);

  virtual uint64_t getTrueFootprint() const = 0;
  virtual uint64_t getFootprint(unsigned cacheBlockSize) const = 0;
  virtual bool isContinuous() const = 0;

  LLVMTraceCPU *getCPU() { return this->cpu; }
  GemForgeCPUDelegator *getCPUDelegator() { return this->cpuDelegator; }

  virtual void configure(uint64_t seqNum) = 0;

  void dispatchStreamConfig(uint64_t seqNum);
  void executeStreamConfig(uint64_t seqNum);
  bool isStreamConfigureExecuted(uint64_t configInstSeqNum);
  void commitStreamEnd(uint64_t seqNum);

  /**
   * Called by executeStreamConfig() to allow derived class to set up the
   * AddrGenCallback in DynamicStream.
   */
  virtual void setupAddrGen(DynamicStream &dynStream) = 0;

  /**
   * ! Sean: StreamAwareCache
   * Allocate the CacheStreamConfigureData.
   */
  virtual CacheStreamConfigureData *
  allocateCacheConfigureData(uint64_t configSeqNum) = 0;

  /**
   * Helper function used in StreamAwareCache.
   */
  virtual bool isDirectLoadStream() const { return false; }
  virtual bool isPointerChaseLoadStream() const { return false; }
  virtual uint64_t getStreamLengthAtInstance(uint64_t streamInstance) const = 0;

  std::deque<DynamicStream> dynamicStreams;
  DynamicStream &getDynamicStream(uint64_t seqNum);

protected:
  LLVMTraceCPU *cpu;
  GemForgeCPUDelegator *cpuDelegator;
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

  /**
   * For debug.
   */
  virtual void dump() const = 0;
};

#endif