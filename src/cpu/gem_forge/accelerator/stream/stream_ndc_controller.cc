#include "stream_ndc_controller.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...) SE_DPRINTF_(StreamEngine, format, ##args)

#include "debug/StreamNearDataComputing.hh"
#define DEBUG_TYPE StreamNearDataComputing
#include "stream_log.hh"

StreamNDCController::StreamNDCController(StreamEngine *_se) : se(_se) {}

void StreamNDCController::offloadStreams(
    const StreamConfigArgs &args, const ::LLVM::TDG::StreamRegion &region,
    DynStreamList &dynStreams) {

  /**
   * We first offload single operand compute streams.
   */
  for (auto dynS : dynStreams) {
    auto S = dynS->stream;
    if (S->isAtomicComputeStream()) {
      dynS->offloadedAsNDC = true;
      S->statistic.numFineGrainedOffloaded++;
    }
  }
}

void StreamNDCController::issueNDCPacket(StreamElement *element) {
  auto dynS = element->dynS;
  auto S = element->stream;

  if (!dynS->offloadedAsNDC) {
    return;
  }

  S_ELEMENT_DPRINTF(element, "Create NDC Packet.\n");
  auto *NDCPacketVec = new StreamNDCPacketVec();

  Addr paddr;
  if (!this->se->cpuDelegator->translateVAddrOracle(element->addr, paddr)) {
    S_ELEMENT_PANIC(element, "Fault on NDC Packet VAddr %#x.\n", element->addr);
  }
  auto NDCPacket = std::make_shared<StreamNDCPacket>(S, element->FIFOIdx,
                                                     element->addr, paddr);
  NDCPacketVec->push_back(NDCPacket);

  // Send all NDC packets in one message.
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, this->se->cpuDelegator->dataMasterId(), 0,
      MemCmd::Command::StreamNDCReq, reinterpret_cast<uint64_t>(NDCPacketVec));
  this->se->cpuDelegator->sendRequest(pkt);
}