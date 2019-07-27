#include "stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#define STREAM_DPRINTF(format, args...)                                        \
  DPRINTF(StreamEngine, "Stream %s: " format, this->getStreamName().c_str(),   \
          ##args)

#define STREAM_ENTRY_DPRINTF(entry, format, args...)                           \
  STREAM_DPRINTF("Entry (%lu, %lu): " format, (entry).idx.streamInstance,      \
                 (entry).idx.entryIdx, ##args)

#define STREAM_HACK(format, args...)                                           \
  hack("Stream %s: " format, this->getStreamName().c_str(), ##args)

#define STREAM_ENTRY_HACK(entry, format, args...)                              \
  STREAM_HACK("Entry (%lu, %lu): " format, (entry).idx.streamInstance,         \
              (entry).idx.entryIdx, ##args)

#define STREAM_PANIC(format, args...)                                          \
  {                                                                            \
    this->dump();                                                              \
    panic("Stream %s: " format, this->getStreamName().c_str(), ##args);        \
  }

#define STREAM_ENTRY_PANIC(entry, format, args...)                             \
  STREAM_PANIC("Entry (%lu, %lu): " format, (entry).idx.streamInstance,        \
               (entry).idx.entryIdx, ##args)

Stream::Stream(const StreamArguments &args)
    : cpu(args.cpu), se(args.se), nilTail(args.se) {

  this->configured = false;
  this->head = &this->nilTail;
  this->stepped = &this->nilTail;
  this->tail = &this->nilTail;
  this->allocSize = 0;
  this->stepSize = 0;
  this->maxSize = args.maxSize;
  this->stepRootStream = nullptr;
  this->lateFetchCount = 0;
  this->streamRegion = args.streamRegion;

  /**
   * Initialize the statistics.
   */
  this->numConfigured = 0;
  this->numAllocated = 0;
  this->numFetched = 0;
  this->numStepped = 0;
  this->numUsed = 0;
  this->numIssuedRequest = 0;
  this->numMissL0 = 0;
  this->numMissL1 = 0;
  this->numMissL2 = 0;
}

Stream::~Stream() {}

void Stream::dumpStreamStats(std::ostream &os) const {
  os << this->getStreamName() << '\n';
#define dumpScalar(stat) os << "  " #stat << ' ' << this->stat << '\n'

  dumpScalar(numConfigured);
  dumpScalar(numAllocated);
  dumpScalar(numFetched);
  dumpScalar(numStepped);
  dumpScalar(numUsed);
  dumpScalar(numIssuedRequest);
  dumpScalar(numMissL0);
  dumpScalar(numMissL1);
  dumpScalar(numMissL2);

#undef dumpScalar
}

bool Stream::isMemStream() const {
  return this->getStreamType() == "load" || this->getStreamType() == "store";
}

void Stream::addBaseStream(Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStreams.insert(baseStream);
  baseStream->dependentStreams.insert(this);
}

void Stream::addBackBaseStream(Stream *backBaseStream) {
  if (backBaseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->backBaseStreams.insert(backBaseStream);
}

void Stream::addBaseStepStream(Stream *baseStepStream) {
  if (baseStepStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStepStreams.insert(baseStepStream);
  baseStepStream->dependentStepStreams.insert(this);
  if (baseStepStream->isStepRoot()) {
    this->baseStepRootStreams.insert(baseStepStream);
  } else {
    for (auto stepRoot : baseStepStream->baseStepRootStreams) {
      this->baseStepRootStreams.insert(stepRoot);
    }
  }
}

void Stream::registerStepDependentStreamToRoot(Stream *newStepDependentStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Try to register step instruction to non-root stream.");
  }
  for (auto &stepStream : this->stepStreamList) {
    if (stepStream == newStepDependentStream) {
      STREAM_PANIC(
          "The new step dependent stream has already been registered.");
    }
  }
  this->stepStreamList.emplace_back(newStepDependentStream);
}

void Stream::dispatchStreamConfigure(StreamConfigInst *inst) {
  this->configInstExecuted.emplace_back(inst->getSeqNum(), false);
}

void Stream::executeStreamConfigure(StreamConfigInst *inst) {
  for (auto &record : this->configInstExecuted) {
    if (record.first == inst->getSeqNum()) {
      assert(record.second == false &&
             "This StreamConfig is alread executed for this stream.");
      record.second = true;
      return;
    }
  }
  assert(false && "Failed to find the record for the StreamConfig inst.");
}

bool Stream::isStreamConfigureExecuted(uint64_t configInstSeqNum) {
  for (auto &record : this->configInstExecuted) {
    if (record.first == configInstSeqNum) {
      return record.second;
    }
  }
  assert(false && "Failed to find the record for the StreamConfig seqNum.");
}

void Stream::commitStreamEnd(StreamEndInst *inst) {
  assert(!this->configInstExecuted.empty() && "Empty list for StreamEnd.");
  assert(this->configInstExecuted.front().first < inst->getSeqNum() &&
         "End before config.");
  assert(this->configInstExecuted.front().second &&
         "End before config executed.");
  this->configInstExecuted.pop_front();
  if (!this->configInstExecuted.empty()) {
    // There is another configinst waiting.
    assert(this->configInstExecuted.front().first > inst->getSeqNum() &&
           "Next StreamConfig not younger than previous StreamEnd.");
  }
}