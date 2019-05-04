#include "stream.hh"
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

Stream::Stream(const StreamArguments &args)
    : cpu(args.cpu), se(args.se), nilTail(args.se),
      firstConfigSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
      endSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM), storedData(nullptr) {

  this->storedData = new uint8_t[cpu->system->cacheLineSize()];

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
}

Stream::~Stream() {
  // Actually the stream is only deallocated at the end of the program.
  // But we still release the memory for completeness.
  if (this->storedData != nullptr) {
    delete[] this->storedData;
    this->storedData = nullptr;
  }

  for (auto memAccess : this->memAccesses) {
    delete memAccess;
  }
  this->memAccesses.clear();
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

uint64_t Stream::getCacheBlockAddr(uint64_t addr) const {
  return addr & (~(cpu->system->cacheLineSize() - 1));
}

void Stream::addAliveCacheBlock(uint64_t addr) const {
  if (this->getStreamType() == "phi") {
    return;
  }
  auto cacheBlockAddr = this->getCacheBlockAddr(addr);
  if (this->aliveCacheBlocks.count(cacheBlockAddr) == 0) {
    this->aliveCacheBlocks.emplace(cacheBlockAddr, 1);
  } else {
    this->aliveCacheBlocks.at(cacheBlockAddr)++;
  }
}

bool Stream::isCacheBlockAlive(uint64_t addr) const {
  if (this->getStreamType() == "phi") {
    return false;
  }
  auto cacheBlockAddr = this->getCacheBlockAddr(addr);
  return this->aliveCacheBlocks.count(cacheBlockAddr) != 0;
}

void Stream::removeAliveCacheBlock(uint64_t addr) const {
  if (this->getStreamType() == "phi") {
    return;
  }
  auto cacheBlockAddr = this->getCacheBlockAddr(addr);
  auto aliveMapIter = this->aliveCacheBlocks.find(cacheBlockAddr);
  if (aliveMapIter == this->aliveCacheBlocks.end()) {
    STREAM_PANIC("Missing alive cache block.");
  } else {
    if (aliveMapIter->second == 1) {
      this->aliveCacheBlocks.erase(aliveMapIter);
    } else {
      aliveMapIter->second--;
    }
  }
}
void Stream::StreamMemAccess::handlePacketResponse(LLVMTraceCPU *cpu,
                                                   PacketPtr packet) {
  if (this->additionalDelay == 0) {
    this->stream->handlePacketResponse(this->entryId, packet, this);
  } else {
    // Schedule the event and clear the result.
    Cycles delay(this->additionalDelay);
    // Remember to clear this additional delay as we have already paid it.
    this->additionalDelay = 0;
    auto responseEvent = new ResponseEvent(cpu, this, packet);
    cpu->schedule(responseEvent, cpu->clockEdge(delay));
  }
}

void Stream::StreamMemAccess::handlePacketResponse(PacketPtr packet) {
  if (this->additionalDelay == 0) {
    this->stream->handlePacketResponse(this->entryId, packet, this);
  } else {
    // Schedule the event and clear the result.
    auto cpu = this->stream->getCPU();
    Cycles delay(this->additionalDelay);
    // Remember to clear this additional delay as we have already paid it.
    this->additionalDelay = 0;
    auto responseEvent = new ResponseEvent(cpu, this, packet);
    cpu->schedule(responseEvent, cpu->clockEdge(delay));
  }
}
