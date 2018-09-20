#include "stream.hh"
#include "stream_engine.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#define STREAM_DPRINTF(format, args...)                                        \
  DPRINTF(StreamEngine, "Stream %s: " format, this->getStreamName().c_str(),   \
          ##args)

#define STREAM_PANIC(format, args...)                                          \
  panic("Stream %s: " format, this->getStreamName().c_str(), ##args)

Stream::Stream(const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst,
               LLVMTraceCPU *_cpu, StreamEngine *_se)
    : cpu(_cpu), se(_se), pattern(configInst.pattern_path()),
      storedData(nullptr), RUN_AHEAD_FIFO_ENTRIES(10) {

  const auto &streamName = configInst.stream_name();
  const auto &streamId = configInst.stream_id();
  const auto &infoPath = configInst.info_path();
  ProtoInputStream infoIStream(infoPath);
  if (!infoIStream.read(this->info)) {
    STREAM_PANIC(
        "Failed to read in the stream info for stream %s from file %s.",
        streamName.c_str(), infoPath.c_str());
  }

  if (this->info.name() != streamName) {
    STREAM_PANIC(
        "Mismatch of stream name from stream config instruction (%s) and "
        "info file (%s).",
        streamName.c_str(), this->info.name().c_str());
  }
  if (this->info.id() != streamId) {
    STREAM_PANIC(
        "Mismatch of stream id from stream config instruction (%lu) and "
        "info file (%lu).",
        streamId, this->info.id());
  }

  for (const auto &baseStreamId : this->info.chosen_base_ids()) {
    auto baseStream = this->se->getStreamNullable(baseStreamId);
    if (baseStream == nullptr) {
      STREAM_PANIC("Failed to get base stream, is it not initialized yet.");
    }
    this->addBaseStream(baseStream);
  }

  if (this->info.type() == "store") {
    this->storedData = new uint8_t[this->info.element_size()];
  }

  STREAM_DPRINTF("Initialized.\n");
}

Stream::~Stream() {
  // Actually the stream is only deallocated at the end of the program.
  // But we still release the memory for completeness.
  if (this->storedData != nullptr) {
    delete[] this->storedData;
    this->storedData = nullptr;
  }

  for (auto memInst : this->memInsts) {
    delete memInst;
  }
  this->memInsts.clear();
}

void Stream::addBaseStream(Stream *baseStream) {
  if (baseStream == this) {
    STREAM_PANIC("Base stream should not be self.");
  }
  this->baseStreams.insert(baseStream);
  baseStream->dependentStreams.insert(this);
}

void Stream::configure() {
  STREAM_DPRINTF("Configured.\n");
  this->pattern.configure();

  this->FIFO.clear();
  this->FIFOIdx = 0;
  while (this->FIFO.size() < this->RUN_AHEAD_FIFO_ENTRIES) {
    this->enqueueFIFO();
  }
}

void Stream::store(uint64_t userSeqNum) {
  STREAM_DPRINTF("Stored.\n");
  if (this->FIFO.empty()) {
    STREAM_PANIC("Store when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }

  if (this->storedData == nullptr) {
    STREAM_PANIC("StoredData is nullptr for store stream.");
  }

  auto &entry = this->findCorrectUsedEntry(userSeqNum);

  /**
   * Send the write packet with random data.
   */
  auto memInst = new StreamMemAccessInst(this, entry.idx);
  this->memInsts.insert(memInst);
  auto paddr = cpu->translateAndAllocatePhysMem(entry.value);
  cpu->sendRequest(paddr, this->info.element_size(), memInst, storedData);

  entry.store();
}

Stream::FIFOEntry &Stream::findCorrectUsedEntry(uint64_t userSeqNum) {
  for (auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      // This entry has not been stepped.
      return entry;
    } else if (entry.stepSeqNum > userSeqNum) {
      // This entry is already stepped, but the stepped inst is younger than the
      // user, so the user should use this entry.
      return entry;
    }
  }
  STREAM_PANIC("Failed to find the correct used entry.");
}

const Stream::FIFOEntry &
Stream::findCorrectUsedEntry(uint64_t userSeqNum) const {
  for (const auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      // This entry has not been stepped.
      return entry;
    } else if (entry.stepSeqNum > userSeqNum) {
      // This entry is already stepped, but the stepped inst is younger than the
      // user, so the user should use this entry.
      return entry;
    }
  }
  STREAM_PANIC("Failed to find the correct used entry.");
}

bool Stream::isReady(uint64_t userSeqNum) const {
  if (this->FIFO.empty()) {
    return false;
  }
  const auto &entry = this->findCorrectUsedEntry(userSeqNum);
  return entry.valid;
}

bool Stream::canStep() const {
  for (const auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
  }
  return false;
}

void Stream::enqueueFIFO() {
  auto nextValuePair = this->pattern.getNextValue();
  STREAM_DPRINTF("Enqueue with idx %lu value (%s, %lu), fifo size %lu.\n",
                 this->FIFOIdx, (nextValuePair.first ? "valid" : "invalid"),
                 nextValuePair.second, this->FIFO.size());
  this->FIFO.emplace_back(this->FIFOIdx++, nextValuePair.second);

  /**
   * If the next value is not valid, we mark the entry valid immediately, as it
   * should be accessed, otherwise, we will send the packet if this is memory
   * stream.
   */
  auto &entry = this->FIFO.back();
  if (!nextValuePair.first) {
    entry.markReady(cpu->curCycle());
  } else {
    if (this->info.type() == "phi") {
      // This is an IVStream.
      this->triggerReady(this, entry.idx);
      entry.markReady(cpu->curCycle());
    } else if (this->info.type() == "load") {
      /**
       * This is a load stream, create the mem inst.
       */
      STREAM_DPRINTF("Send packet for entry %lu with addr %p.\n", entry.idx,
                     reinterpret_cast<void *>(entry.value));
      auto memInst = new StreamMemAccessInst(this, entry.idx);
      this->memInsts.insert(memInst);
      auto paddr = cpu->translateAndAllocatePhysMem(entry.value);
      cpu->sendRequest(paddr, this->info.element_size(), memInst, nullptr);

    } else if (this->info.type() == "store") {
      /**
       * This is a store stream. Also send the load request to bring up the
       * cache line.
       * Store stream is always ready.
       */
      entry.markReady(cpu->curCycle());

      auto memInst = new StreamMemAccessInst(this, entry.idx);
      this->memInsts.insert(memInst);
      auto paddr = cpu->translateAndAllocatePhysMem(entry.value);
      cpu->sendRequest(paddr, this->info.element_size(), memInst, nullptr);
    } else {
      STREAM_PANIC("Unrecognized stream type %s.", this->info.type().c_str());
    }
  }
}

void Stream::handlePacketResponse(uint64_t entryId, PacketPtr packet,
                                  StreamMemAccessInst *memInst) {
  if (this->memInsts.count(memInst) == 0) {
    STREAM_PANIC("Failed looking up the stream memory access inst in our set.");
  }

  /**
   * If I am a load stream, mark the entry as ready now.
   */
  if (this->info.type() == "load") {
    // bool foundEntryInFIFO = false;
    for (auto &entry : this->FIFO) {
      if (entry.idx == entryId) {
        // We actually ingore the data here.
        // foundEntryInFIFO = true;
        entry.markReady(cpu->curCycle());
        this->triggerReady(this, entryId);
        STREAM_DPRINTF("Received load stream packet for entry %lu.\n", entryId);
      }
    }
    // It is possible that the entry is already stepped before the packet
    // returns, if the entry is unused.
  } else if (this->info.type() == "store") {
    // This is a store stream.
    // Noting to do.
  } else {
    STREAM_PANIC("Invalid type %s for a stream to receive packet response.",
                 this->info.type().c_str());
  }

  this->memInsts.erase(memInst);
  delete memInst;
}

void Stream::triggerReady(Stream *rootStream, uint64_t entryId) {
  for (auto &dependentStream : this->dependentStreams) {
    STREAM_DPRINTF("Trigger ready entry %lu root %s stream %s.\n", entryId,
                   rootStream->getStreamName().c_str(),
                   dependentStream->getStreamName().c_str());
    dependentStream->receiveReady(rootStream, this, entryId);
  }
}

void Stream::receiveReady(Stream *rootStream, Stream *baseStream,
                          uint64_t entryId) {
  if (this->baseStreams.count(baseStream) == 0) {
    STREAM_PANIC("Received ready signal from illegal base stream.");
  }
  if (rootStream == this) {
    STREAM_PANIC("Dependence cycle detected.");
  }
  STREAM_DPRINTF("Received ready signal for entry %lu from stream %s.\n",
                 entryId, baseStream->getStreamName().c_str());
}

void Stream::step(uint64_t stepSeqNum) {
  if (!this->baseStreams.empty()) {
    STREAM_PANIC(
        "Receive step signal from nowhere for a stream with base streams.");
  }
  this->stepImpl(stepSeqNum);
  // Send out the step signal as the root stream.
  this->triggerStep(stepSeqNum, this);
}

void Stream::triggerStep(uint64_t stepSeqNum, Stream *rootStream) {
  for (auto &dependentStream : this->dependentStreams) {
    STREAM_DPRINTF("Trigger step root %s stream %s.\n",
                   rootStream->getStreamName().c_str(),
                   dependentStream->getStreamName().c_str());
    dependentStream->receiveStep(stepSeqNum, rootStream, this);
  }
}

void Stream::receiveStep(uint64_t stepSeqNum, Stream *rootStream,
                         Stream *baseStream) {
  if (this->baseStreams.count(baseStream) == 0) {
    STREAM_PANIC("Received step signal from illegal base stream.");
  }
  if (rootStream == this) {
    STREAM_PANIC("Dependence cycle detected.");
  }
  STREAM_DPRINTF("Received step signal for entry %lu from stream %s.\n",
                 baseStream->getStreamName().c_str());
  this->stepImpl(stepSeqNum);
  this->triggerStep(stepSeqNum, rootStream);
}

void Stream::stepImpl(uint64_t stepSeqNum) {
  STREAM_DPRINTF("Stepped.\n");
  if (this->FIFO.empty()) {
    STREAM_PANIC("Step when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }
  for (auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      entry.step(stepSeqNum);
      return;
    }
  }
  STREAM_PANIC("Failed to find available entry to step.");
}

void Stream::commitStep(uint64_t stepSeqNum) {
  if (!this->baseStreams.empty()) {
    STREAM_PANIC("Receive commit step signal from nowhere for a stream with "
                 "base streams.");
  }
  this->commitStepImpl(stepSeqNum);
  // Send out the step signal as the root stream.
  this->triggerCommitStep(stepSeqNum, this);
}

void Stream::triggerCommitStep(uint64_t stepSeqNum, Stream *rootStream) {
  for (auto &dependentStream : this->dependentStreams) {
    STREAM_DPRINTF("Trigger commit step root %s stream %s.\n",
                   rootStream->getStreamName().c_str(),
                   dependentStream->getStreamName().c_str());
    dependentStream->receiveCommitStep(stepSeqNum, rootStream, this);
  }
}

void Stream::receiveCommitStep(uint64_t stepSeqNum, Stream *rootStream,
                               Stream *baseStream) {
  if (this->baseStreams.count(baseStream) == 0) {
    STREAM_PANIC("Received commit step signal from illegal base stream.");
  }
  if (rootStream == this) {
    STREAM_PANIC("Dependence cycle detected.");
  }
  STREAM_DPRINTF("Received commit step signal for entry %lu from stream %s.\n",
                 baseStream->getStreamName().c_str());
  this->commitStepImpl(stepSeqNum);
  this->triggerCommitStep(stepSeqNum, rootStream);
}

void Stream::commitStepImpl(uint64_t stepSeqNum) {
  STREAM_DPRINTF("Commit stepped.\n");
  if (this->FIFO.empty()) {
    STREAM_PANIC("Commit step when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }
  if (this->FIFO.front().stepSeqNum != stepSeqNum) {
    STREAM_PANIC("Unmatched stepSeqNum for entry %lu (%lu) with %lu.",
                 this->FIFO.front().idx, this->FIFO.front().stepSeqNum,
                 stepSeqNum);
  }
  this->FIFO.pop_front();
  while (this->FIFO.size() < this->RUN_AHEAD_FIFO_ENTRIES) {
    this->enqueueFIFO();
  }
}

LLVM::TDG::TDGInstruction Stream::StreamMemAccessInst::dummyTDGInstruction;

void Stream::StreamMemAccessInst::handlePacketResponse(LLVMTraceCPU *cpu,
                                                       PacketPtr packet) {
  this->stream->handlePacketResponse(this->entryId, packet, this);
}

void Stream::FIFOEntry::store() {
  if (this->stored) {
    panic("This entry %lu has already been stored before.", this->idx);
  }
  this->stored = true;
}

void Stream::FIFOEntry::step(uint64_t stepSeqNum) {
  if (this->stepSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM) {
    panic("This entry %lu has already been stepped before.", this->idx);
  }
  this->stepSeqNum = stepSeqNum;
}