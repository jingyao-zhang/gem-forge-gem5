#include "single_stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/misc.hh"
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

SingleStream::SingleStream(LLVMTraceCPU *_cpu, StreamEngine *_se,
                           const LLVM::TDG::StreamInfo &_info,
                           size_t _maxRunAHeadLength)
    : Stream(_cpu, _se, _maxRunAHeadLength), info(_info) {
  const auto &relativeHistoryPath = this->info.history_path();
  auto historyPath = cpu->getTraceExtraFolder() + "/" + relativeHistoryPath;
  this->history =
      std::unique_ptr<StreamHistory>(new StreamHistory(historyPath));
  this->patternStream = std::unique_ptr<StreamPattern>(new StreamPattern(
      cpu->getTraceExtraFolder() + "/" + this->info.pattern_path()));

  for (const auto &baseStreamId : this->info.chosen_base_streams()) {
    auto baseStream = this->se->getStream(baseStreamId.id());
    this->addBaseStream(baseStream);
  }

  if (this->baseStreams.empty() && this->info.type() == "phi") {
    this->stepRootStream = this;
  }

  // Try to find the step root stream.
  for (auto &baseS : this->baseStreams) {
    if (baseS->getLoopLevel() != this->getLoopLevel()) {
      continue;
    }
    if (baseS->stepRootStream != nullptr) {
      if (this->stepRootStream != nullptr &&
          this->stepRootStream != baseS->stepRootStream) {
        panic("Double step root stream found.\n");
      }
      this->stepRootStream = baseS->stepRootStream;
    }
  }

  STREAM_DPRINTF("Initialized.\n");
}

SingleStream::~SingleStream() {}

const std::string &SingleStream::getStreamName() const {
  return this->info.name();
}

const std::string &SingleStream::getStreamType() const {
  return this->info.type();
}

uint32_t SingleStream::getLoopLevel() const { return this->info.loop_level(); }

uint32_t SingleStream::getConfigLoopLevel() const {
  return this->info.config_loop_level();
}

int32_t SingleStream::getElementSize() const {
  return this->info.element_size();
}

void SingleStream::configure(StreamConfigInst *inst) {
  this->history->configure();
  this->patternStream->configure();
}

void SingleStream::prepareNewElement(StreamElement *element) {
  bool oracleUsed = false;
  auto nextValuePair = this->history->getNextAddr(oracleUsed);
  element->addr = nextValuePair.second;
  element->size = this->getElementSize();
}

void SingleStream::handlePacketResponse(const FIFOEntryIdx &entryId,
                                        PacketPtr packet,
                                        StreamMemAccess *memAccess) {
  panic("Not working so far.");
  // if (this->memAccesses.count(memAccess) == 0) {
  //   STREAM_PANIC("Failed looking up the stream memory access inst in our
  //   set.");
  // }

  // this->se->numStreamMemRequests++;

  // /**
  //  * If I am a load stream, mark the entry as value ready now.
  //  * It is possible that the entry is already stepped before the packet
  //  * returns, if the entry is unused.
  //  *
  //  * If I am a store stream, do nothing.
  //  */
  // if (this->info.type() == "load") {
  //   for (auto &entry : this->FIFO) {
  //     if (entry.idx == entryId) {
  //       // We actually ingore the data here.
  //       STREAM_ENTRY_DPRINTF(entry, "Received load stream packet.\n");
  //       if (entry.inflyLoadPackets == 0) {
  //         STREAM_ENTRY_PANIC(entry, "Received load stream packet when there
  //         is "
  //                                   "no infly load packets.");
  //       }
  //       entry.inflyLoadPackets--;
  //       if (entry.inflyLoadPackets == 0) {
  //         this->markValueReady(entry);
  //       }
  //     }
  //   }
  // } else if (this->info.type() == "store") {
  // } else {
  //   STREAM_PANIC("Invalid type %s for a stream to receive packet response.",
  //                this->info.type().c_str());
  // }

  // this->memAccesses.erase(memAccess);
  // delete memAccess;
}

uint64_t SingleStream::getTrueFootprint() const {
  return this->history->getNumCacheLines();
}

uint64_t SingleStream::getFootprint(unsigned cacheBlockSize) const { return 1; }

bool SingleStream::isContinuous() const { return false; }

void SingleStream::dump() const {
  inform("Dump for stream %s.\n======================",
         this->getStreamName().c_str());
  inform("ConfigSeq %lu, EndSeq %lu.\n", this->configSeqNum, this->endSeqNum);
  inform("=========================\n");
}