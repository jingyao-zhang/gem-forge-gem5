#include "stream_ndc_controller.hh"

#define SE_DPRINTF_(X, format, args...)                                        \
  DPRINTF(X, "[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)
#define SE_DPRINTF(format, args...)                                            \
  SE_DPRINTF_(StreamEngineBase, format, ##args)

#define SE_PANIC(X, format, args...)                                           \
  PANIC("[SE%d]: " format, this->se->cpuDelegator->cpuId(), ##args)

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
      dynS->setFloatedAsNDC(true);
      S->statistic.numFineGrainedOffloaded++;
    } else if (S->isStoreComputeStream()) {
      /**
       * We also offload ValueBase LoadStreams without core user.
       */
      bool canOffload = true;
      DynStreamList valueBaseDynStreams;
      for (auto baseEdge : dynS->baseEdges) {
        if (baseEdge.type != DynStream::StreamDepEdge::TypeE::Value) {
          continue;
        }
        auto valueBaseS = se->getStream(baseEdge.baseStaticId);
        auto &valueBaseDynS =
            valueBaseS->getDynStreamByInstance(baseEdge.baseInstanceId);
        if (valueBaseS->hasCoreUser()) {
          canOffload = false;
          break;
        }
        if (!valueBaseS->isLoadStream() || valueBaseS->isLoadComputeStream()) {
          canOffload = false;
          break;
        }
        if (valueBaseDynS.configSeqNum != dynS->configSeqNum) {
          canOffload = false;
          break;
        }
        valueBaseDynStreams.push_back(&valueBaseDynS);
      }
      if (canOffload) {
        dynS->setFloatedAsNDC(true);
        S->statistic.numFineGrainedOffloaded++;
        // All the load value base streams are offloaded as NDCForward.
        for (auto valueBaseDynS : valueBaseDynStreams) {
          valueBaseDynS->setFloatedAsNDC(true);
          valueBaseDynS->setFloatedAsNDCForward(true);
        }
      }
    }
  }
}

bool StreamNDCController::canIssueNDCPacket(StreamElement *element) {
  auto S = element->stream;
  auto dynS = element->dynS;
  if (!dynS->isFloatedAsNDC() || dynS->isFloatedAsNDCForward()) {
    return true;
  }
  /**
   * This is used to check NDCForward elements are address ready for
   * StoreComputeStream.
   */
  if (S->isStoreComputeStream()) {
    for (auto &valueBaseElement : element->valueBaseElements) {
      auto sendingElement = valueBaseElement.getElement();
      if (sendingElement == element) {
        // Avoid self dependence.
        continue;
      }
      if (!sendingElement->isAddrReady()) {
        S_ELEMENT_DPRINTF(element,
                          "Cannot Issuing NDC Packet as NDCForward Element %s "
                          "Not AddrReady.\n",
                          sendingElement->FIFOIdx);
        return false;
      }
    }
  } else if (S->isAtomicComputeStream()) {
    for (auto &valueBaseElement : element->valueBaseElements) {
      auto E = valueBaseElement.getElement();
      if (E == element) {
        // Avoid self dependence.
        continue;
      }
      if (!E->isValueReady) {
        S_ELEMENT_DPRINTF(element,
                          "Cannot Issuing NDC Packet as ValueBase Element %s "
                          "Not ValueReady.\n",
                          E->FIFOIdx);
        return false;
      }
    }
  }
  return true;
}

void StreamNDCController::issueNDCPacket(StreamElement *element) {
  auto dynS = element->dynS;
  auto S = element->stream;

  if (!dynS->isFloatedAsNDC() || dynS->isFloatedAsNDCForward()) {
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
  // Set up the atomic op.
  if (S->isAtomicComputeStream()) {
    auto getBaseValue = [element](Stream::StaticId id) -> StreamValue {
      return element->getValueBaseByStreamId(id);
    };
    auto atomicOp = S->setupAtomicOp(element->FIFOIdx, element->size,
                                     dynS->storeFormalParams, getBaseValue);
    NDCPacket->atomicOp = std::move(atomicOp);
  } else if (S->isStoreComputeStream()) {
    // Create forward NDC packet.
    std::unordered_set<StreamElement *> sentElements;
    for (auto &valueBaseElement : element->valueBaseElements) {
      if (valueBaseElement.getElement() == element) {
        // Avoid self dependence.
        continue;
      }
      auto sendingElement = valueBaseElement.getElement();
      if (sentElements.count(sendingElement)) {
        continue;
      }
      if (!sendingElement->isAddrReady()) {
        S_ELEMENT_PANIC(sendingElement,
                        "NDCForward element is not AddrReady yet.");
      }
      Addr sendingPAddr;
      if (!this->se->cpuDelegator->translateVAddrOracle(sendingElement->addr,
                                                        sendingPAddr)) {
        S_ELEMENT_PANIC(sendingElement,
                        "Fault on NDCForward Packet VAddr %#x.\n",
                        sendingElement->addr);
      }
      auto valueBaseNDCPacket = std::make_shared<StreamNDCPacket>(
          sendingElement->stream, sendingElement->FIFOIdx, sendingElement->addr,
          sendingPAddr);
      S_ELEMENT_DPRINTF(sendingElement, "Create NDCForward Packet.\n");

      valueBaseNDCPacket->isForward = true;
      valueBaseNDCPacket->receiverEntryIdx = element->FIFOIdx;
      valueBaseNDCPacket->receiverVAddr = element->addr;
      valueBaseNDCPacket->receiverPAddr = paddr;
      NDCPacket->expectedForwardPackets.push_back(valueBaseNDCPacket);

      NDCPacketVec->push_back(valueBaseNDCPacket);
      sentElements.insert(sendingElement);
    }
  } else {
    // Don't know how to handle this NDC packet.
    S_ELEMENT_PANIC(element, "Illegal StreamType for NDC.");
  }
  NDCPacketVec->push_back(NDCPacket);

  // Send all NDC packets in one message.
  Addr initPAddr = 0;
  auto pkt = GemForgePacketHandler::createStreamControlPacket(
      initPAddr, this->se->cpuDelegator->dataMasterId(), 0,
      MemCmd::Command::StreamNDCReq, reinterpret_cast<uint64_t>(NDCPacketVec));
  this->se->cpuDelegator->sendRequest(pkt);
}