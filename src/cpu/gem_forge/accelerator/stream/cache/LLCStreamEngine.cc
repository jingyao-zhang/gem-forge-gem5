
#include "LLCStreamEngine.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "base/trace.hh"
#include "debug/RubyStream.hh"

LLCStreamEngine::LLCStreamEngine(AbstractStreamAwareController *_controller)
    : Consumer(_controller), controller(_controller), issueWidth(4),
      migrateWidth(2) {}

LLCStreamEngine::~LLCStreamEngine() {
  for (auto &s : this->streams) {
    delete s;
    s = nullptr;
  }
  this->streams.clear();
}

void LLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  DPRINTF(RubyStream,
          "LLCStreamEngine: Received Pkt %#x, StreamConfigure %#x, initVAddr "
          "%#x, "
          "initPAddr %#x.\n",
          pkt, streamConfigureData, streamConfigureData->initVAddr,
          streamConfigureData->initPAddr);
  // Create the stream.
  this->streams.emplace_back(new LLCDynamicStream(streamConfigureData));
  // Release memory.
  delete streamConfigureData;
  delete pkt;

  // Let's schedule a wakeup event.
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::wakeup() {
  DPRINTF(RubyStream, "LLCStreamEngine wake up.\n");
  this->issueStreams();
  this->migrateStreams();
}

void LLCStreamEngine::issueStreams() {
  DPRINTF(RubyStream, "LLCStreamEngine issueStreams.\n");
  auto streamIter = this->streams.begin();
  auto streamEnd = this->streams.end();
  StreamList issuedStreams;
  while (streamIter != streamEnd && issuedStreams.size() < this->issueWidth) {
    auto stream = *streamIter;
    bool issued = this->issueStream(stream);
    if (issued) {
      // Check if we want to migrate the stream.
      issuedStreams.emplace_back(stream);
      streamIter = this->streams.erase(streamIter);
    } else {
      // Move to the next one.
      ++streamIter;
    }
  }

  for (auto stream : issuedStreams) {
    // Peek the next physical address.
    auto nextVAddr = stream->peekVAddr();
    auto nextPAddr = stream->translateToPAddr(nextVAddr);
    // Check if it is still on this bank.
    bool shouldMigrate = !this->isPAddrHandledByMe(nextPAddr);
    if (shouldMigrate) {
      this->migratingStreams.emplace_back(stream);
    } else {
      this->streams.emplace_back(stream);
    }
  }
}

bool LLCStreamEngine::issueStream(LLCDynamicStream *stream) {

  /**
   * Key point is to merge continuous stream elements within one cache line.
   * TODO: Really check if continuous. So far just consume until a different
   * TODO: cache line.
   */
  if (!stream->isNextElementAllcoated()) {
    return false;
  }

  // Get the first element.
  int numElements = 0;
  Addr vaddr = stream->peekVAddr();
  Addr paddr = stream->translateToPAddr(vaddr);
  Addr paddrLine = makeLineAddress(paddr);
  inform("%#x, \n", paddr);
  assert(this->isPAddrHandledByMe(paddr) && "This should never happen.");
  stream->consumeNextElement();
  numElements = 1;

  // Try to get more elements.
  while (stream->isNextElementAllcoated()) {
    Addr nextVAddr = stream->peekVAddr();
    Addr nextPAddr = stream->translateToPAddr(nextVAddr);
    Addr nextPAddrLine = makeLineAddress(nextPAddr);
    if (nextPAddrLine == paddrLine) {
      // We can merge the request.
      stream->consumeNextElement();
      numElements++;
    } else {
      break;
    }
  }

  DPRINTF(RubyStream, "LLCStreamEngine issue stream %s, numElement %d.\n",
          stream->getStaticStream()->getStreamName().c_str(), numElements);

  // TODO: Really issue the request.
  return true;
}

void LLCStreamEngine::migrateStreams() {
  DPRINTF(RubyStream, "LLCStreamEngine migrateStreams.\n");
  auto streamIter = this->migratingStreams.begin();
  auto streamEnd = this->migratingStreams.end();
  int migrated = 0;
  while (streamIter != streamEnd && migrated < this->migrateWidth) {
    auto stream = *streamIter;
    this->migrateStream(stream);
    streamIter = this->migratingStreams.erase(streamIter);
    migrated++;
  }
}

void LLCStreamEngine::migrateStream(LLCDynamicStream *stream) {}

bool LLCStreamEngine::isPAddrHandledByMe(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddr, selfMachineId.type);
  inform("%d, %d\n", selfMachineId.getNum(), addrMachineId.getNum());
  return addrMachineId == selfMachineId;
}

void LLCStreamEngine::print(std::ostream &out) const {}