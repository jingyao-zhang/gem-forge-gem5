#include "coalesced_stream.hh"
#include "stream_engine.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

// #include "base/misc.hh""
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

#include <sstream>

#define LOGICAL_STREAM_PANIC(S, format, args...)                               \
  panic("Logical Stream %s: " format, (S)->info.name().c_str(), ##args)

LogicalStream::LogicalStream(const std::string &_traceExtraFolder,
                             const LLVM::TDG::StreamInfo &_info)
    : info(_info) {
  this->history = std::unique_ptr<StreamHistory>(
      new StreamHistory(_traceExtraFolder + "/" + this->info.history_path()));
  this->patternStream = std::unique_ptr<StreamPattern>(
      new StreamPattern(_traceExtraFolder + "/" + this->info.pattern_path()));
}

LogicalStream::~LogicalStream() {}

CoalescedStream::CoalescedStream(const StreamArguments &args,
                                 const LLVM::TDG::StreamInfo &_primaryInfo)
    : Stream(args) {
  // Create the primary logical stream.
  this->addStreamInfo(_primaryInfo);
  // Set the primaryLogicalStream to the newly created one.
  this->primaryLogicalStream = &(this->coalescedStreams.front());
}

CoalescedStream::~CoalescedStream() {}

void CoalescedStream::addStreamInfo(const LLVM::TDG::StreamInfo &info) {
  /**
   * Note: At this point the primary logical stream may not be created yet!
   */
  this->coalescedStreams.emplace_back(cpu->getTraceExtraFolder(), info);
  this->generateStreamName();
  // Update the dependence information.
  for (const auto &baseStreamId : info.chosen_base_streams()) {
    auto baseStream = this->se->getStream(baseStreamId.id());
    assert(baseStream != this && "Should never have circular dependency.");
    this->addBaseStream(baseStream);
  }

  for (const auto &backBaseStreamId : info.chosen_back_base_streams()) {
    auto backBaseStream = this->se->getStream(backBaseStreamId.id());
    assert(backBaseStream != this && "Should never have circular dependency.");
    this->addBackBaseStream(backBaseStream);
  }

  assert(info.type() != "phi" && "Never coalesce phi stream.");

  // Try to update the step root stream.
  for (auto &baseS : this->baseStreams) {
    if (baseS->getLoopLevel() != info.loop_level()) {
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
}

void CoalescedStream::initializeBackBaseStreams() {
  for (auto &logicalStream : this->coalescedStreams) {
    auto &info = logicalStream.info;
    assert(info.chosen_back_base_streams_size() == 0 &&
           "No back edge dependence for coalesced stream.");
  }
}

void CoalescedStream::configure(StreamConfigInst *inst) {
  for (auto &S : this->coalescedStreams) {
    S.history->configure();
    S.patternStream->configure();
  }
}

void CoalescedStream::prepareNewElement(StreamElement *element) {
  std::list<uint64_t> cacheBlocks;
  /**
   * The size of the coalesced element [lhsAddr, rhsAddr).
   * ? The dummy initializer to silence the compiler.
   */
  Addr lhsAddr = 0, rhsAddr = 4;
  for (auto &S : this->coalescedStreams) {
    bool oracleUsed = false;
    auto nextValuePair = S.history->getNextAddr(oracleUsed);
    if (!nextValuePair.first) {
      continue;
    }
    auto addr = nextValuePair.second;
    auto elementSize = S.info.element_size();
    if (cacheBlocks.empty()) {
      lhsAddr = addr;
      rhsAddr = addr + elementSize;
    } else {
      lhsAddr = (addr < lhsAddr) ? addr : lhsAddr;
      rhsAddr =
          ((addr + elementSize) > rhsAddr) ? (addr + elementSize) : rhsAddr;
    }

    const int cacheBlockSize = cpu->system->cacheLineSize();
    auto lhsCacheBlock = addr & (~(cacheBlockSize - 1));
    auto rhsCacheBlock = (addr + elementSize - 1) & (~(cacheBlockSize - 1));
    while (lhsCacheBlock <= rhsCacheBlock) {
      if (cacheBlocks.size() > StreamElement::MAX_CACHE_BLOCKS) {
        inform(
            "%s: More than %d cache blocks for one stream element, address %lu "
            "size %lu.",
            this->getStreamName().c_str(), cacheBlocks.size(), addr,
            S.info.element_size());
      }

      // Insert into the list.
      auto cacheBlockIter = cacheBlocks.begin();
      auto cacheBlockEnd = cacheBlocks.end();
      bool inserted = false;
      while (cacheBlockIter != cacheBlockEnd) {
        auto cacheBlock = *cacheBlockIter;
        if (cacheBlock == lhsCacheBlock) {
          inserted = true;
          break;
        } else if (cacheBlock > lhsCacheBlock) {
          cacheBlocks.insert(cacheBlockIter, lhsCacheBlock);
          inserted = true;
          break;
        }
        ++cacheBlockIter;
      }
      if (!inserted) {
        cacheBlocks.push_back(lhsCacheBlock);
      }

      if (lhsCacheBlock == 0xFFFFFFFFFFFFFFC0) {
        // This is the last block in the address space.
        // Something wrong here.
        break;
      }
      lhsCacheBlock += cacheBlockSize;
    }
  }

  if (cacheBlocks.size() > StreamElement::MAX_CACHE_BLOCKS) {
    panic("%s: More than %d cache blocks for one stream element",
          this->getStreamName().c_str(), cacheBlocks.size());
  }

  // Check if all the cache blocks are continuous.
  if (cacheBlocks.size() > 1) {
    auto initCacheBlock = cacheBlocks.front();
    auto idx = 0;
    for (auto cacheBlock : cacheBlocks) {
      if (cacheBlock != initCacheBlock + idx * cpu->system->cacheLineSize()) {
        for (auto c : cacheBlocks) {
          hack("Uncontinuous address for coalesced stream %lx\n", c);
        }
        panic("Uncontinuous address for coalesced stream %s.",
              this->getStreamName().c_str());
      }
      ++idx;
    }
  }
  // Add the stream.
  if (cacheBlocks.empty()) {
    element->addr = 0;
    element->size = 4;
  } else {
    element->addr = lhsAddr;
    element->size = rhsAddr - lhsAddr;
    // element->addr = cacheBlocks.front();
    // element->size = cacheBlocks.size() * cpu->system->cacheLineSize();
  }
}

void CoalescedStream::generateStreamName() {
  std::stringstream ss;
  ss << "(CLS ";
  const auto &primaryName = this->coalescedStreams.front().info.name();
  auto lhs = primaryName.find(' ');
  auto rhs = primaryName.rfind(' ');
  ss << primaryName.substr(lhs + 1, rhs - lhs - 1);

  for (const auto &S : this->coalescedStreams) {
    const auto &name = S.info.name();
    auto pos = name.rfind(' ');
    ss << ' ' << name.substr(pos + 1, name.size() - pos - 2);
  }
  ss << ")";
  this->streamName = ss.str();
}

const std::string &CoalescedStream::getStreamName() const {
  return this->streamName;
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

bool CoalescedStream::isContinuous() const {
  const auto &pattern = this->primaryLogicalStream->patternStream->getPattern();
  if (pattern.val_pattern() != "LINEAR") {
    return false;
  }
  return this->getElementSize() == pattern.stride_i();
}

uint64_t CoalescedStream::getFootprint(unsigned cacheBlockSize) const {
  /**
   * Estimate the memory footprint for this stream in number of unqiue cache
   * blocks. It is OK for us to under-estimate the footprint, as the cache will
   * try to cache a stream with low-memory footprint.
   */
  const auto &pattern = this->primaryLogicalStream->patternStream->getPattern();
  const auto totalElements =
      this->primaryLogicalStream->history->getCurrentStreamLength();
  if (pattern.val_pattern() == "LINEAR") {
    // One dimension linear stream.
    return totalElements * pattern.stride_i() / cacheBlockSize;
  } else if (pattern.val_pattern() == "QUARDRIC") {
    // For 2 dimention linear stream, first compute footprint of one row.
    auto rowFootprint = pattern.ni() * this->getElementSize() / cacheBlockSize;
    if (pattern.stride_i() > cacheBlockSize) {
      rowFootprint = pattern.ni();
    }
    /**
     * Now we check if there is any chance that the next row will overlap with
     * the previous row.
     */
    auto rowRange = std::abs(pattern.stride_i()) * pattern.ni();
    if (std::abs(pattern.stride_j()) < rowRange) {
      // There is a chance that the next row will overlap with the previous one.
      // Return one row footprint as an under-estimation.
      return rowFootprint;
    } else {
      // No chance of overlapping.
      return rowFootprint * (totalElements / pattern.ni());
    }
  } else {
    // For all other patterns, underestimate.
    return 1;
  }
}

uint64_t CoalescedStream::getTrueFootprint() const {
  return this->primaryLogicalStream->history->getNumCacheLines();
}

void CoalescedStream::dump() const {
  inform("Dump for coalesced stream %s.\n======================",
         this->getStreamName().c_str());
  inform("=========================\n");
}
