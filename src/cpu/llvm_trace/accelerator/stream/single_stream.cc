#include "single_stream.hh"
#include "insts.hh"
#include "stream_engine.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"

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

SingleStream::SingleStream(
    const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst,
    LLVMTraceCPU *_cpu, StreamEngine *_se, bool _isOracle,
    size_t _maxRunAHeadLength, const std::string &_throttling)
    : Stream(_cpu, _se, _isOracle, _maxRunAHeadLength, _throttling) {

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

  this->history = std::unique_ptr<StreamHistory>(
      new StreamHistory(this->info.history_path()));

  for (const auto &baseStreamId : this->info.chosen_base_ids()) {
    auto baseStream = this->se->getStreamNullable(baseStreamId);
    if (baseStream == nullptr) {
      STREAM_PANIC("Failed to get base stream, is it not initialized yet.");
    }
    this->addBaseStream(baseStream);
  }

  for (const auto &baseStepStreamId : this->info.chosen_base_step_ids()) {
    auto baseStepStream = this->se->getStreamNullable(baseStepStreamId);
    if (baseStepStream == nullptr) {
      STREAM_PANIC(
          "Failed to get base step stream, is it not initialized yet.");
    }
    this->addBaseStepStream(baseStepStream);
  }
  if (this->baseStepRootStreams.size() > 1) {
    STREAM_PANIC(
        "More than one base step root stream detected, which is not yet "
        "supported by the semantics of step instructions.");
  }
  if (!this->isStepRoot()) {
    for (auto &stepRootStream : this->baseStepRootStreams) {
      stepRootStream->registerStepDependentStreamToRoot(this);
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
  Stream::configure(inst);
}

void SingleStream::enqueueFIFO() {

  bool oracleUsed = false;
  auto nextValuePair = this->history->getNextAddr(oracleUsed);
  STREAM_DPRINTF(
      "Enqueue with idx (%lu, %lu) value (%s, %lu), fifo size %lu.\n",
      this->FIFOIdx.streamInstance, this->FIFOIdx.entryIdx,
      (nextValuePair.first ? "valid" : "invalid"), nextValuePair.second,
      this->FIFO.size());
  this->FIFO.emplace_back(this->FIFOIdx, oracleUsed, nextValuePair.second,
                          this->info.element_size(),
                          LLVMDynamicInst::INVALID_SEQ_NUM);
  this->FIFOIdx.next();

  /**
   * Update the stats.
   */
  this->se->numElements++;
  if (this->isMemStream()) {
    this->se->numMemElements++;
  }

  auto &entry = this->FIFO.back();

  /**
   * Check if the base values are valid, which determins if our current entry is
   * ready. For streams without base streams, this will always return true.
   */
  if (this->checkIfEntryBaseValuesValid(entry)) {
    this->markAddressReady(entry);
  }
}

void SingleStream::handlePacketResponse(const FIFOEntryIdx &entryId,
                                        PacketPtr packet,
                                        StreamMemAccess *memAccess) {
  if (this->memAccesses.count(memAccess) == 0) {
    STREAM_PANIC("Failed looking up the stream memory access inst in our set.");
  }

  this->se->numStreamMemRequests++;

  /**
   * If I am a load stream, mark the entry as value ready now.
   * It is possible that the entry is already stepped before the packet
   * returns, if the entry is unused.
   *
   * If I am a store stream, do nothing.
   */
  if (this->info.type() == "load") {
    for (auto &entry : this->FIFO) {
      if (entry.idx == entryId) {
        // We actually ingore the data here.
        STREAM_ENTRY_DPRINTF(entry, "Received load stream packet.\n");
        if (entry.inflyLoadPackets == 0) {
          STREAM_ENTRY_PANIC(entry, "Received load stream packet when there is "
                                    "no infly load packets.");
        }
        entry.inflyLoadPackets--;
        if (entry.inflyLoadPackets == 0) {
          this->markValueReady(entry);
        }
      }
    }
  } else if (this->info.type() == "store") {
  } else {
    STREAM_PANIC("Invalid type %s for a stream to receive packet response.",
                 this->info.type().c_str());
  }

  this->memAccesses.erase(memAccess);
  delete memAccess;
}

void SingleStream::markAddressReady(FIFOEntry &entry) {

  if (entry.isAddressValid) {
    STREAM_ENTRY_PANIC(entry, "The entry is already address ready.");
  }

  STREAM_ENTRY_DPRINTF(entry, "Mark address ready.\n");
  entry.markAddressReady(cpu->curCycle());

  if (this->info.type() == "phi") {
    // For IV stream, the value is immediately ready.
    this->markValueReady(entry);
    return;
  }

  if (this->isOracle) {
    // If we are oracle, and the entry is unused, immediately mark it value
    // ready without sending the packet.
    // if (!entry.oracleUsed) {
    //   this->markValueReady(entry);
    //   return;
    // }
    this->markValueReady(entry);
    return;
  }

  // After this point, we are going to fetch the data from cache.
  se->numMemElementsFetched++;

  // Start to construct the packets.
  auto size = entry.size;
  for (int packetSize, inflyPacketsSize = 0, packetIdx = 0;
       inflyPacketsSize < size; inflyPacketsSize += packetSize, packetIdx++) {
    Addr paddr, vaddr;
    if (cpu->isStandalone()) {
      vaddr = entry.address + inflyPacketsSize;
      paddr = cpu->translateAndAllocatePhysMem(vaddr);
    } else {
      STREAM_PANIC("Stream so far can only work in standalone mode.");
    }
    packetSize = size - inflyPacketsSize;
    // Do not span across cache line.
    auto cacheLineSize = cpu->system->cacheLineSize();
    if (((paddr % cacheLineSize) + packetSize) > cacheLineSize) {
      packetSize = cacheLineSize - (paddr % cacheLineSize);
    }

    // Construct the packet.
    if (this->info.type() == "load") {
      /**
       * This is a load stream, create the mem inst.
       */
      STREAM_ENTRY_DPRINTF(
          entry, "Send load packet #%d with addr %p, size %d.\n", packetIdx,
          reinterpret_cast<void *>(vaddr), packetSize);
      auto memAccess = new StreamMemAccess(this, entry.idx);
      this->memAccesses.insert(memAccess);
      cpu->sendRequest(paddr, packetSize, memAccess, nullptr);

      entry.inflyLoadPackets++;
    } else if (this->info.type() == "store") {
      /**
       * This is a store stream. Also send the load request to bring up the
       * cache line.
       */
      STREAM_ENTRY_DPRINTF(
          entry, "Send store fetch packet #d with addr %p, size %d.\n",
          packetIdx, reinterpret_cast<void *>(vaddr), packetSize);
      auto memAccess = new StreamMemAccess(this, entry.idx);
      this->memAccesses.insert(memAccess);
      cpu->sendRequest(paddr, packetSize, memAccess, nullptr);
    }
  }

  if (this->info.type() == "store") {
    // Store stream is always value ready.
    this->markValueReady(entry);
  }
}

void SingleStream::markValueReady(FIFOEntry &entry) {
  if (entry.isValueValid) {
    STREAM_ENTRY_PANIC(entry, "The entry is already value ready.");
  }
  STREAM_ENTRY_DPRINTF(entry, "Mark value ready.\n");
  for (int i = 0; i < entry.cacheBlocks; ++i) {
    this->addAliveCacheBlock(entry.cacheBlockAddrs[i]);
  }
  entry.markValueReady(cpu->curCycle());

  // Check if there is already some user waiting for this entry.
  if (!entry.users.empty()) {
    auto waitCycles = entry.valueReadyCycles - entry.firstCheckIfReadyCycles;
    se->entryWaitCycles += waitCycles;
    if (this->isMemStream()) {
      se->memEntryWaitCycles += waitCycles;
    }
  }

  this->triggerReady(this, entry.idx);
}

void SingleStream::dump() const {
  inform("Dump for stream %s.\n======================",
         this->getStreamName().c_str());
  inform("ConfigSeq %lu, EndSeq %lu.\n", this->configSeqNum, this->endSeqNum);
  for (const auto &entry : this->FIFO) {
    entry.dump();
  }
  for (const auto &userEntryPair : this->userToEntryMap) {
    inform("user %lu entry (%lu, %lu)\n", userEntryPair.first,
           userEntryPair.second->idx.streamInstance,
           userEntryPair.second->idx.entryIdx);
  }
  inform("=========================\n");
}