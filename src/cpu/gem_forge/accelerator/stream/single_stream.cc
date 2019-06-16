#include "single_stream.hh"
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

SingleStream::SingleStream(const StreamArguments &args,
                           const LLVM::TDG::StreamInfo &_info)
    : Stream(args), info(_info) {
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

void SingleStream::initializeBackBaseStreams() {
  for (const auto &backBaseStreamId : this->info.chosen_back_base_streams()) {
    assert(this->getStreamType() == "phi" &&
           "Only phi node can have back edge dependence.");
    auto backBaseStream = this->se->getStream(backBaseStreamId.id());
    this->addBackBaseStream(backBaseStream);
  }
}

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
  this->dispatchStreamConfigure(inst);
  this->history->configure();
  this->patternStream->configure();
}

void SingleStream::prepareNewElement(StreamElement *element) {
  bool oracleUsed = false;
  auto nextValuePair = this->history->getNextAddr(oracleUsed);
  element->addr = nextValuePair.second;
  element->size = this->getElementSize();
}

uint64_t SingleStream::getTrueFootprint() const {
  return this->history->getNumCacheLines();
}

uint64_t SingleStream::getFootprint(unsigned cacheBlockSize) const { return 1; }

bool SingleStream::isContinuous() const { return false; }

void SingleStream::dump() const {
  inform("Dump for stream %s.\n======================",
         this->getStreamName().c_str());
  inform("=========================\n");
}