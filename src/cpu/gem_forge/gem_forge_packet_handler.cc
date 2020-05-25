#include "gem_forge_packet_handler.hh"

GemForgePacketReleaseHandler GemForgePacketReleaseHandler::instance;

PacketPtr GemForgePacketHandler::createGemForgePacket(
    Addr paddr, int size, GemForgePacketHandler *handler, uint8_t *data,
    MasterID masterID, int contextId, Addr pc) {
  RequestPtr req(new Request(paddr, size, 0, masterID,
                             reinterpret_cast<InstSeqNum>(handler), contextId));
  if (pc != 0) {
    req->setPC(pc);
  }
  // For our request, we always track the request statistic.
  req->setStatistic(std::make_shared<RequestStatistic>());
  PacketPtr pkt;
  uint8_t *pkt_data = new uint8_t[req->getSize()];
  if (data == nullptr) {
    pkt = Packet::createRead(req);
  } else {
    pkt = Packet::createWrite(req);
    // Copy the value to store.
    memcpy(pkt_data, data, req->getSize());
  }
  pkt->dataDynamic(pkt_data);
  // Push the handler as the SenderState.
  pkt->pushSenderState(handler);
  return pkt;
}

PacketPtr GemForgePacketHandler::createGemForgeAMOPacket(
    Addr vaddr, Addr paddr, int size, MasterID masterID, int contextId, Addr pc,
    AtomicOpFunctor *atomicOp) {
  Request::Flags flags;
  flags.set(Request::ATOMIC_RETURN_OP);

  int asid = 0;
  RequestPtr req = std::make_shared<Request>(asid, vaddr, size, flags, masterID,
                                             pc, contextId, atomicOp);
  req->setPaddr(paddr);
  // For our request, we always track the request statistic.
  req->setStatistic(std::make_shared<RequestStatistic>());
  PacketPtr pkt = Packet::createWrite(req);
  // Fake some data.
  uint8_t *pkt_data = new uint8_t[req->getSize()];
  pkt->dataDynamic(pkt_data);
  // Push the dummy release handler as the SenderState.
  auto handler = GemForgePacketReleaseHandler::get();
  pkt->pushSenderState(handler);
  return pkt;
}

PacketPtr GemForgePacketHandler::createStreamControlPacket(Addr paddr,
                                                           MasterID masterID,
                                                           int contextId,
                                                           MemCmd::Command cmd,
                                                           uint64_t data) {
  /**
   * ! Pure evil hack here.
   * ! Pass the data in pktData.
   */
  RequestPtr req(
      new Request(paddr, sizeof(uint64_t), 0, masterID, 1, contextId));
  PacketPtr pkt = new Packet(req, cmd);
  uint8_t *pktData = new uint8_t[req->getSize()];
  *(reinterpret_cast<uint64_t *>(pktData)) = data;
  pkt->dataDynamic(pktData);
  return pkt;
}

bool GemForgePacketHandler::isGemForgePacket(PacketPtr pkt) {
  auto handler = pkt->findNextSenderState<GemForgePacketHandler>();
  return handler != nullptr;
}

void GemForgePacketHandler::handleGemForgePacketResponse(
    GemForgeCPUDelegator *cpuDelegator, PacketPtr pkt) {
  // Decode the handler information.
  auto handler = pkt->findNextSenderState<GemForgePacketHandler>();
  assert(handler != NULL && "This is not a GemForgePacket.");
  handler->handlePacketResponse(cpuDelegator, pkt);
}

void GemForgePacketHandler::issueToMemory(GemForgeCPUDelegator *cpuDelegator,
                                          PacketPtr pkt) {
  // Decode the handler information.
  auto handler = pkt->findNextSenderState<GemForgePacketHandler>();
  if (handler == nullptr) {
    // This is not a GemForgePacket. Ignore it.
    return;
  }
  handler->issueToMemoryCallback(cpuDelegator);
}

bool GemForgePacketHandler::needResponse(PacketPtr pkt) {
  // Decode the handler information.
  // TODO: Make a real GemForgePacketHandler for StreamControl packet.
  // TODO: Fix this implementation. Too hacky.
  auto handler = pkt->findNextSenderState<GemForgePacketHandler>();
  if (handler == nullptr) {
    // This is not a GemForgePacket. Check if its command is special command.
    // StreamConfig/End requests have no response.
    if (pkt->cmd == MemCmd::Command::StreamConfigReq) {
      return false;
    } else if (pkt->cmd == MemCmd::Command::StreamEndReq) {
      return false;
    } else {
      // This is not a good idea maybe.
      assert(false && "Normal packet encountered by GemForgePacketHandler.");
      return true;
    }
  }
  // So far all GemForgePacketHandler requires response.
  return true;
}