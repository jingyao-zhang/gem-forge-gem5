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

#define STREAM_ENTRY_DPRINTF(entry, format, args...)                           \
  STREAM_DPRINTF("Entry (%lu, %lu): " format, (entry).idx.streamInstance,      \
                 (entry).idx.entryIdx, ##args)

#define STREAM_PANIC(format, args...)                                          \
  panic("Stream %s: " format, this->getStreamName().c_str(), ##args)

#define STREAM_ENTRY_PANIC(entry, format, args...)                             \
  STREAM_PANIC("Entry (%lu, %lu): " format, (entry).idx.streamInstance,        \
               (entry).idx.entryIdx, ##args)

Stream::Stream(const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst,
               LLVMTraceCPU *_cpu, StreamEngine *_se)
    : cpu(_cpu), se(_se), firstConfigSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM), storedData(nullptr),
      RUN_AHEAD_FIFO_ENTRIES(10) {

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

void Stream::configure(uint64_t configSeqNum) {
  STREAM_DPRINTF("Configured at set num %lu.\n", configSeqNum);
  this->history->configure();
  this->configSeqNum = configSeqNum;
  if (this->firstConfigSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
    this->firstConfigSeqNum = configSeqNum;
  }

  // For all the entries without stepSeqNum, this means that these entries are
  // speculative run ahead. Clear them out.
  auto FIFOIter = this->FIFO.begin();
  while (FIFOIter != this->FIFO.end()) {
    if (FIFOIter->stepped()) {
      // This one has already stepped.
      ++FIFOIter;
    } else {
      // This one should be cleared.
      STREAM_ENTRY_DPRINTF(*FIFOIter, "Clear run-ahead entry.\n");
      FIFOIter = this->FIFO.erase(FIFOIter);
    }
  }

  // Reset the FIFOIdx.
  this->FIFOIdx.newInstance();
  while (this->FIFO.size() < this->RUN_AHEAD_FIFO_ENTRIES) {
    this->enqueueFIFO();
  }
}

void Stream::store(uint64_t storeSeqNum) {
  STREAM_DPRINTF("Stored with seqNum %lu.\n", storeSeqNum);
  if (this->FIFO.empty()) {
    STREAM_PANIC("Store when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }

  if (this->storedData == nullptr) {
    STREAM_PANIC("StoredData is nullptr for store stream.");
  }

  auto entry = this->findCorrectUsedEntry(storeSeqNum);
  if (entry == nullptr) {
    STREAM_PANIC("Try to store when there is no available entry. Something "
                 "wrong in isReady.");
  }

  if (entry->stored()) {
    STREAM_ENTRY_PANIC(*entry, "entry is already stored.");
  }
  entry->store(storeSeqNum);

  /**
   * For store stream, if there is no base step stream, which means this is a
   * constant store or somehow, we can step it now.
   */
  if (this->isStepRoot()) {
    // Implicitly step the stream.
    this->step(storeSeqNum);
  }
}

void Stream::commitStore(uint64_t storeSeqNum) {
  STREAM_DPRINTF("Store committed with seq %lu.\n", storeSeqNum);
  if (this->FIFO.empty()) {
    STREAM_PANIC("Commit store when the FIFO is empty.");
  }
  auto &entry = this->FIFO.front();
  if (entry.storeSeqNum != storeSeqNum) {
    STREAM_ENTRY_PANIC(
        entry, "Mismatch between the store seq num %lu with entry (%lu).",
        storeSeqNum, entry.storeSeqNum);
  }
  // Now actually send the committed data.
  if (this->storedData == nullptr) {
    STREAM_PANIC("StoredData is nullptr for store stream.");
  }

  /**
   * Send the write packet with random data.
   */
  if (entry.value != 0) {
    auto memInst = new StreamMemAccessInst(this, entry.idx);
    this->memInsts.insert(memInst);
    auto paddr = cpu->translateAndAllocatePhysMem(entry.value);
    STREAM_DPRINTF("Send stream store packet at %p size %d.\n",
                   reinterpret_cast<void *>(entry.value),
                   this->info.element_size());
    cpu->sendRequest(paddr, this->info.element_size(), memInst, storedData);
  }

  /**
   * Implicitly commit the step if we have no base stream.
   */
  // if (storeSeqNum == 5742) {
  //   STREAM_PANIC("Jesus found.\n");
  // }
  if (this->isStepRoot()) {
    this->commitStep(storeSeqNum);
  }
}

Stream::FIFOEntry *Stream::findCorrectUsedEntry(uint64_t userSeqNum) {
  for (auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      // This entry has not been stepped.
      return &entry;
    } else if (entry.stepSeqNum > userSeqNum) {
      // This entry is already stepped, but the stepped inst is younger than the
      // user, so the user should use this entry.
      return &entry;
    }
  }
  return nullptr;
}

const Stream::FIFOEntry *
Stream::findCorrectUsedEntry(uint64_t userSeqNum) const {
  for (const auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      // This entry has not been stepped.
      return &entry;
    } else if (entry.stepSeqNum > userSeqNum) {
      // This entry is already stepped, but the stepped inst is younger than the
      // user, so the user should use this entry.
      return &entry;
    }
  }
  return nullptr;
}

bool Stream::isReady(uint64_t userSeqNum) const {
  if (this->FIFO.empty()) {
    return false;
  }
  if (userSeqNum == 27250) {
    STREAM_DPRINTF("Check is ready for 27250.\n");
    for (const auto &entry : this->FIFO) {
      STREAM_ENTRY_DPRINTF(entry, "stepSeq %lu, addrValid %d, valValid %d\n",
                           entry.stepSeqNum, entry.isAddressValid,
                           entry.isValueValid);
    }
  }
  // STREAM_DPRINTF("Check if stream is ready for inst %lu.\n", userSeqNum);
  const auto *entry = this->findCorrectUsedEntry(userSeqNum);
  // if (entry != nullptr) {
  //   STREAM_ENTRY_DPRINTF(*entry, "Check if entry is ready %d.\n",
  //                        entry->isValueValid);
  // } else {
  //   STREAM_DPRINTF("Failed to find the entry.\n");
  // }
  return (entry != nullptr) && (entry->isValueValid);
}

void Stream::use(uint64_t userSeqNum) {
  if (!this->isReady(userSeqNum)) {
    STREAM_PANIC("Try to use stream when the we are not ready.");
  }
  auto *entry = this->findCorrectUsedEntry(userSeqNum);
  STREAM_ENTRY_DPRINTF(*entry, "Used by %lu.\n", userSeqNum);
  if (entry->firstUseCycles == LLVMDynamicInst::INVALID_SEQ_NUM) {
    // Update the stats.
    entry->firstUseCycles = cpu->curCycle();
    this->se->numElementsUsed++;
  }
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
  auto nextValuePair = this->history->getNextAddr();
  STREAM_DPRINTF(
      "Enqueue with idx (%lu, %lu) value (%s, %lu), fifo size %lu.\n",
      this->FIFOIdx.streamInstance, this->FIFOIdx.entryIdx,
      (nextValuePair.first ? "valid" : "invalid"), nextValuePair.second,
      this->FIFO.size());
  this->FIFO.emplace_back(this->FIFOIdx, nextValuePair.second,
                          LLVMDynamicInst::INVALID_SEQ_NUM);
  this->FIFOIdx.next();

  /**
   * Update the stats.
   */
  this->se->numElements++;

  auto &entry = this->FIFO.back();

  /**
   * Check if the base values are valid, which determins if our current entry is
   * ready. For streams without base streams, this will always return true.
   */
  if (this->checkIfEntryBaseValuesValid(entry)) {
    this->markAddressReady(entry);
  }
}

bool Stream::checkIfEntryBaseValuesValid(const FIFOEntry &entry) const {
  const auto &myLoopLevel = this->info.loop_level();
  const auto &myConfigLoopLevel = this->info.config_loop_level();
  for (const auto &baseStream : this->baseStreams) {
    // So far we only check the base streams that have the same loop_level and
    // configure_level.
    if (baseStream->info.config_loop_level() != myConfigLoopLevel ||
        baseStream->info.loop_level() != myLoopLevel) {
      continue;
    }

    // If the perfect aligned stream doesn't have step inst, it is a constant
    // stream. We simply assume it's ready now.
    if (baseStream->baseStepRootStreams.empty()) {
      continue;
    }

    // If we are here, that means our FIFO is perfectly aligned.
    bool foundAlignedBaseEntry = false;
    for (const auto &baseEntry : baseStream->FIFO) {
      if (baseEntry.idx == entry.idx) {
        // We found the correct base entry to use.
        if (!baseEntry.isValueValid) {
          return false;
        }
        foundAlignedBaseEntry = true;
        break;
      }
      if (baseEntry.idx.streamInstance > entry.idx.streamInstance) {
        // The base stream is already configured into the next instance.
        // We will soon be configured and flushed. Simply return not ready.
        return false;
      }
    }
    if (!foundAlignedBaseEntry) {
      STREAM_ENTRY_PANIC(entry,
                         "Failed to find the aligned base entry from the "
                         "perfectly aligned base stream %s.\n",
                         baseStream->getStreamName().c_str());
    }
  }
  return true;
}

void Stream::handlePacketResponse(const FIFOEntryIdx &entryId, PacketPtr packet,
                                  StreamMemAccessInst *memInst) {
  if (this->memInsts.count(memInst) == 0) {
    STREAM_PANIC("Failed looking up the stream memory access inst in our set.");
  }

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

  this->memInsts.erase(memInst);
  delete memInst;
}

void Stream::markAddressReady(FIFOEntry &entry) {

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

  // Start to construct the packets.
  auto size = this->info.element_size();
  for (int packetSize, inflyPacketsSize = 0, packetIdx = 0;
       inflyPacketsSize < size; inflyPacketsSize += packetSize, packetIdx++) {
    Addr paddr, vaddr;
    if (cpu->isStandalone()) {
      vaddr = entry.address;
      paddr = cpu->translateAndAllocatePhysMem(vaddr);
    } else {
      STREAM_PANIC("Stream so far can only work in standalone mode.");
    }
    // For now only support maximum 8 bytes access.
    packetSize = size - inflyPacketsSize;
    // if (packetSize > 8) {
    //   packetSize = 8;
    // }
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
      auto memInst = new StreamMemAccessInst(this, entry.idx);
      this->memInsts.insert(memInst);
      cpu->sendRequest(paddr, packetSize, memInst, nullptr);

      entry.inflyLoadPackets++;

    } else if (this->info.type() == "store") {
      /**
       * This is a store stream. Also send the load request to bring up the
       * cache line.
       */
      STREAM_ENTRY_DPRINTF(
          entry, "Send store fetch packet #d with addr %p, size %d.\n",
          packetIdx, reinterpret_cast<void *>(vaddr), packetSize);
      auto memInst = new StreamMemAccessInst(this, entry.idx);
      this->memInsts.insert(memInst);
      cpu->sendRequest(paddr, packetSize, memInst, nullptr);
    }
  }

  if (this->info.type() == "store") {
    // Store stream is always value ready.
    this->markValueReady(entry);
  }
}

void Stream::markValueReady(FIFOEntry &entry) {
  if (entry.isValueValid) {
    STREAM_ENTRY_PANIC(entry, "The entry is already value ready.");
  }
  STREAM_ENTRY_DPRINTF(entry, "Mark value ready.\n");
  entry.markValueReady(cpu->curCycle());
  this->triggerReady(this, entry.idx);
}

void Stream::triggerReady(Stream *rootStream, const FIFOEntryIdx &entryId) {
  for (auto &dependentStream : this->dependentStreams) {
    STREAM_DPRINTF("Trigger ready entry (%lu, %lu) root %s stream %s.\n",
                   entryId.streamInstance, entryId.entryIdx,
                   rootStream->getStreamName().c_str(),
                   dependentStream->getStreamName().c_str());
    dependentStream->receiveReady(rootStream, this, entryId);
  }
}

void Stream::receiveReady(Stream *rootStream, Stream *baseStream,
                          const FIFOEntryIdx &entryId) {
  if (this->baseStreams.count(baseStream) == 0) {
    STREAM_PANIC("Received ready signal from illegal base stream.");
  }
  if (rootStream == this) {
    STREAM_PANIC("Dependence cycle detected.");
  }
  STREAM_DPRINTF("Received ready signal for entry (%lu, %lu) from stream %s.\n",
                 entryId.streamInstance, entryId.entryIdx,
                 baseStream->getStreamName().c_str());
  // Here we simply do an thorough search for our current entries.
  for (auto &entry : this->FIFO) {
    if (entry.isAddressValid) {
      // This entry already has a valid address.
      continue;
    }
    if (this->checkIfEntryBaseValuesValid(entry)) {
      // We are finally ready.
      this->markAddressReady(entry);
      this->triggerReady(rootStream, entry.idx);
    }
  }
}

void Stream::step(uint64_t stepSeqNum) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Receive step signal from nowhere for a non-root stream.");
  }
  this->stepImpl(stepSeqNum);
  // Send out the step signal as the root stream.
  this->triggerStep(stepSeqNum, this);
}

void Stream::triggerStep(uint64_t stepSeqNum, Stream *rootStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Trigger step signal from a non-root stream.");
  }
  for (auto &dependentStepStream : this->stepStreamList) {
    STREAM_DPRINTF("Trigger step for stream %s.\n",
                   dependentStepStream->getStreamName().c_str());
    dependentStepStream->stepImpl(stepSeqNum);
  }
}

void Stream::stepImpl(uint64_t stepSeqNum) {
  if (this->FIFO.empty()) {
    STREAM_PANIC("Step when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }
  for (auto &entry : this->FIFO) {
    if (entry.stepSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      entry.step(stepSeqNum);
      STREAM_ENTRY_DPRINTF(entry, "Stepped with seqNum %lu.\n", stepSeqNum);
      return;
    }
  }
  STREAM_PANIC("Failed to find available entry to step.");
}

void Stream::commitStep(uint64_t stepSeqNum) {
  if (!this->isStepRoot()) {
    STREAM_PANIC(
        "Receive commit step signal from nowhere for a non-root stream");
  }
  this->commitStepImpl(stepSeqNum);
  // Send out the step signal as the root stream.
  this->triggerCommitStep(stepSeqNum, this);
}

void Stream::triggerCommitStep(uint64_t stepSeqNum, Stream *rootStream) {
  if (!this->isStepRoot()) {
    STREAM_PANIC("Trigger commit step signal from a non-root stream");
  }
  for (auto &dependentStepStream : this->stepStreamList) {
    STREAM_DPRINTF("Trigger commit step seqNum %lu for stream %s.\n",
                   stepSeqNum, dependentStepStream->getStreamName().c_str());
    dependentStepStream->commitStepImpl(stepSeqNum);
  }
}

void Stream::commitStepImpl(uint64_t stepSeqNum) {
  if (this->FIFO.empty()) {
    STREAM_PANIC("Commit step when the fifo is empty for stream %s.",
                 this->getStreamName().c_str());
  }
  auto &entry = this->FIFO.front();
  STREAM_ENTRY_DPRINTF(entry, "Commit stepped with seqNum %lu.\n", stepSeqNum);
  if (entry.stepSeqNum != stepSeqNum) {
    STREAM_ENTRY_PANIC(entry, "Unmatched stepSeqNum for entry %lu with %lu.",
                       entry.stepSeqNum, stepSeqNum);
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
void Stream::FIFOEntry::markAddressReady(Cycles readyCycles) {
  this->isAddressValid = true;
  this->addressReadyCycles = readyCycles;
}

void Stream::FIFOEntry::markValueReady(Cycles readyCycles) {
  if (this->inflyLoadPackets > 0) {
    panic("Mark entry value valid when there is still infly load packets.");
  }
  this->isValueValid = true;
  this->valueReadyCycles = readyCycles;
}

void Stream::FIFOEntry::store(uint64_t storeSeqNum) {
  if (this->storeSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM) {
    panic("This entry (%lu, %lu) has already been stored before.",
          this->idx.streamInstance, this->idx.entryIdx);
  }
  this->storeSeqNum = storeSeqNum;
}

void Stream::FIFOEntry::step(uint64_t stepSeqNum) {
  if (this->stepSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM) {
    panic("This entry (%lu, %lu) has already been stepped before.",
          this->idx.streamInstance, this->idx.entryIdx);
  }
  this->stepSeqNum = stepSeqNum;
}