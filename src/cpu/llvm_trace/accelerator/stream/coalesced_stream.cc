#include "coalesced_stream.hh"
#include "stream_engine.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#define LOGICAL_STREAM_PANIC(S, format, args...)                               \
  panic("Logical Stream %s: " format, (S)->info.name().c_str(), ##args)

LogicalStream::LogicalStream(
    const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst) {

  const auto &streamName = configInst.stream_name();
  const auto &streamId = configInst.stream_id();
  const auto &infoPath = configInst.info_path();
  ProtoInputStream infoIStream(infoPath);
  if (!infoIStream.read(this->info)) {
    panic("Failed to read in the stream info for stream %s from file %s.",
          streamName.c_str(), infoPath.c_str());
  }

  if (this->info.name() != streamName) {
    panic("Mismatch of stream name from stream config instruction (%s) and "
          "info file (%s).",
          streamName.c_str(), this->info.name().c_str());
  }
  if (this->info.id() != streamId) {
    panic("Mismatch of stream id from stream config instruction (%lu) and "
          "info file (%lu).",
          streamId, this->info.id());
  }

  this->history = std::unique_ptr<StreamHistory>(
      new StreamHistory(this->info.history_path()));
  this->patternStream = std::unique_ptr<StreamPattern>(
      new StreamPattern(this->info.pattern_path()));
}

LogicalStream::~LogicalStream() {}

CoalescedStream::CoalescedStream(LLVMTraceCPU *_cpu, StreamEngine *_se,
                                 bool _isOracle, size_t _maxRunAHeadLength,
                                 const std::string &_throttling)
    : Stream(_cpu, _se, _isOracle, _maxRunAHeadLength, _throttling),
      primaryLogicalStream(nullptr) {}

CoalescedStream::~CoalescedStream() {}

void CoalescedStream::addLogicalStreamIfNecessary(
    const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst) {

  const auto &streamId = configInst.stream_id();
  auto logicalStreamMapIter = this->logicalStreamMap.find(streamId);
  if (logicalStreamMapIter != this->logicalStreamMap.end()) {
    // This logical stream has already been added.
    return;
  }

  logicalStreamMapIter =
      this->logicalStreamMap
          .emplace(std::piecewise_construct, std::forward_as_tuple(streamId),
                   std::forward_as_tuple(configInst))
          .first;

  auto &logicalStream = logicalStreamMapIter->second;

  /**
   * TODO: Add sanity check.
   */
  if (logicalStream.info.chosen_base_step_root_ids_size() > 1) {
    LOGICAL_STREAM_PANIC(
        &logicalStream,
        "Coalesced logical stream should have less than 2 base step streams.");
  }

  if (logicalStream.info.type() == "phi") {
    panic("Only support coalesced memory stream.");
  }

  if (this->primaryLogicalStream == nullptr) {
    // First logical stream.
    this->primaryLogicalStream = &logicalStream;

    // Register the base step root streams.
    if (this->primaryLogicalStream->info.chosen_base_step_root_ids_size() ==
        1) {

      const auto &baseStepRootStreamId =
          logicalStream.info.chosen_base_step_root_ids(0);
      auto baseStepRootStream =
          this->se->getStreamNullable(baseStepRootStreamId);

      if (baseStepRootStream == nullptr) {
        panic("Failed to get base step stream, is it not initialized yet?");
      }
      this->addBaseStepStream(baseStepRootStream);

      if (this->baseStepRootStreams.size() != 1) {
        panic("Coalesced stream should have exactly one root step stream.");
      }

      for (auto &stepRootStream : this->baseStepRootStreams) {
        stepRootStream->registerStepDependentStreamToRoot(this);
      }
    }

  } else {

    // if (baseStepStreamId !=
    //     this->primaryLogicalStream->info.chosen_base_step_ids(0)) {
    //   panic("All coalesced logical streams should have the same base step "
    //         "stream.");
    // }
  }
}

const std::string &CoalescedStream::getStreamName() const {
  return this->primaryLogicalStream->info.name();
}

const std::string &CoalescedStream::getStreamType() const {
  return this->primaryLogicalStream->info.type();
}

uint32_t CoalescedStream::getLoopLevel() const {
  return this->primaryLogicalStream->info.loop_level();
}

uint32_t CoalescedStream::getConfigLoopLevel() const {
  return this->primaryLogicalStream->info.config_loop_level();
}

int32_t CoalescedStream::getElementSize() const {
  return this->primaryLogicalStream->info.element_size();
}

bool CoalescedStream::shouldHandleStreamInst(StreamInst *inst) const {
  auto streamId = inst->getStreamId();

  auto logicalStreamMapIter = this->logicalStreamMap.find(streamId);
  if (logicalStreamMapIter == this->logicalStreamMap.end()) {
    panic("Incoming inst of a stream not included for this coalesced stream.");
  }

  if (streamId != this->primaryLogicalStream->info.id()) {
    // We only handle the primary config inst.
    return false;
  }
  return true;
}

void CoalescedStream::configure(StreamConfigInst *inst) {
  if (!this->shouldHandleStreamInst(inst)) {
    return;
  }
  for (auto &logicalStreamPair : this->logicalStreamMap) {
    auto &logicalStream = logicalStreamPair.second;
    logicalStream.history->configure();
    logicalStream.patternStream->configure();
  }
  Stream::configure(inst);
}

void CoalescedStream::commitConfigure(StreamConfigInst *inst) {
  if (!this->shouldHandleStreamInst(inst)) {
    return;
  }
  Stream::commitConfigure(inst);
}

void CoalescedStream::step(StreamStepInst *inst) {
  panic("Coalesced streams should only be memory stream and no step inst.");
}

void CoalescedStream::commitStep(StreamStepInst *inst) {
  panic("Coalesced streams should only be memory stream and no step inst.");
}

void CoalescedStream::store(StreamStoreInst *inst) {
  if (!this->shouldHandleStreamInst(inst)) {
    return;
  }
  Stream::store(inst);
}

void CoalescedStream::commitStore(StreamStoreInst *inst) {
  if (!this->shouldHandleStreamInst(inst)) {
    return;
  }
  Stream::commitStore(inst);
}

void CoalescedStream::end(StreamEndInst *inst) {
  if (!this->shouldHandleStreamInst(inst)) {
    return;
  }
  Stream::end(inst);
}

void CoalescedStream::commitEnd(StreamEndInst *inst) {
  if (!this->shouldHandleStreamInst(inst)) {
    return;
  }
  Stream::commitEnd(inst);
}

void CoalescedStream::enqueueFIFO() {
  bool oracleUsed = false;
  auto nextValuePair =
      this->primaryLogicalStream->history->getNextAddr(oracleUsed);
  this->FIFO.emplace_back(this->FIFOIdx, oracleUsed, nextValuePair.second,
                          this->getElementSize(),
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

void CoalescedStream::handlePacketResponse(const FIFOEntryIdx &entryId,
                                           PacketPtr packet,
                                           StreamMemAccess *memAccess) {
  if (this->memAccesses.count(memAccess) == 0) {
    panic("Failed looking up the stream memory access inst in our set.");
  }

  this->se->numStreamMemRequests++;

  /**
   * If I am a load stream, mark the entry as value ready now.
   * It is possible that the entry is already stepped before the packet
   * returns, if the entry is unused.
   *
   * If I am a store stream, do nothing.
   */
  if (this->primaryLogicalStream->info.type() == "load") {
    for (auto &entry : this->FIFO) {
      if (entry.idx == entryId) {
        // We actually ingore the data here.
        // STREAM_ENTRY_DPRINTF(entry, "Received load stream packet.\n");
        if (entry.inflyLoadPackets == 0) {
          // STREAM_ENTRY_PANIC(entry, "Received load stream packet when there
          // is "
          //                           "no infly load packets.");
          panic("Received load stream packet when there is "
                "no infly load packets.");
        }
        entry.inflyLoadPackets--;
        if (entry.inflyLoadPackets == 0) {
          this->markValueReady(entry);
        }
      }
    }
  } else if (this->primaryLogicalStream->info.type() == "store") {
  } else {
    panic("Invalid type %s for a stream to receive packet response.",
          this->primaryLogicalStream->info.type().c_str());
  }

  this->memAccesses.erase(memAccess);
  delete memAccess;
}

void CoalescedStream::markAddressReady(FIFOEntry &entry) {

  if (entry.isAddressValid) {
    // STREAM_ENTRY_PANIC(entry, "The entry is already address ready.");
    panic("The entry is already address ready.");
  }

  // STREAM_ENTRY_DPRINTF(entry, "Mark address ready.\n");
  entry.markAddressReady(cpu->curCycle());

  if (this->primaryLogicalStream->info.type() == "phi") {
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

  if (this->se->isMergeEnabled()) {
    // Merge the request to the alive cache line.
    if (this->isCacheBlockAlive(entry.address)) {
      this->markValueReady(entry);
      return;
    }
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
      panic("Stream so far can only work in standalone mode.");
    }
    packetSize = size - inflyPacketsSize;
    // Do not span across cache line.
    auto cacheLineSize = cpu->system->cacheLineSize();
    if (((paddr % cacheLineSize) + packetSize) > cacheLineSize) {
      packetSize = cacheLineSize - (paddr % cacheLineSize);
    }

    // Construct the packet.
    if (this->primaryLogicalStream->info.type() == "load") {
      /**
       * This is a load stream, create the mem inst.
       */
      // STREAM_ENTRY_DPRINTF(
      //     entry, "Send load packet #%d with addr %p, size %d.\n", packetIdx,
      //     reinterpret_cast<void *>(vaddr), packetSize);
      auto memAccess = new StreamMemAccess(this, entry.idx);
      this->memAccesses.insert(memAccess);
      cpu->sendRequest(paddr, packetSize, memAccess, nullptr);

      entry.inflyLoadPackets++;

    } else if (this->primaryLogicalStream->info.type() == "store") {
      /**
       * This is a store stream. Also send the load request to bring up the
       * cache line.
       */
      // STREAM_ENTRY_DPRINTF(
      //     entry, "Send store fetch packet #d with addr %p, size %d.\n",
      //     packetIdx, reinterpret_cast<void *>(vaddr), packetSize);
      auto memAccess = new StreamMemAccess(this, entry.idx);
      this->memAccesses.insert(memAccess);
      cpu->sendRequest(paddr, packetSize, memAccess, nullptr);
    }
  }

  if (this->primaryLogicalStream->info.type() == "store") {
    // Store stream is always value ready.
    this->markValueReady(entry);
  }
}

void CoalescedStream::markValueReady(FIFOEntry &entry) {
  if (entry.isValueValid) {
    // STREAM_ENTRY_PANIC(entry, "The entry is already value ready.");
    panic("The entry is already value ready.");
  }
  // STREAM_ENTRY_DPRINTF(entry, "Mark value ready.\n");
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

// std::pair<uint64_t, uint64_t> CoalescedStream::getNextAddr() {
//   bool oracleUsed = false;
//   auto pair = this->primaryLogicalStream->history->getNextAddr(oracleUsed);

//   auto addr = pair.second;
//   auto size = this->primaryLogicalStream->info.element_size();

//   auto cacheBlockAddr = this->getCacheBlockAddr(pair.second);
//   for (auto &logicalStreamPair : this->logicalStreamMap) {
//     auto logicalStream = &(logicalStreamPair.second);
//     bool logicalStreamOracleUsed = false;
//     auto logicalStreamNextPair =
//         logicalStream->history->getNextAddr(logicalStreamOracleUsed);
//     auto logicalStreamCacheBlockAddr =
//         this->getCacheBlockAddr(logicalStreamNextPair.second);
//     auto cacheBlockDiff =
//         std::abs(static_cast<int64_t>(logicalStreamCacheBlockAddr -
//                                       baseCacheBlockAddr)) /
//         cpu->system->cacheLineSize();
//     if (cacheBlockDiff != 1) {
//       continue;
//     }
//     }
// }

uint64_t CoalescedStream::getFootprint(unsigned cacheBlockSize) const {
  return 1;
}

void CoalescedStream::dump() const {
  inform("Dump for coalesced stream %s.\n======================",
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