#include "tdg_packet_handler.hh"

PacketPtr TDGPacketHandler::createTDGPacket(Addr paddr, int size,
                                            TDGPacketHandler *handler,
                                            uint8_t *data, MasterID masterID,
                                            int contextId, Addr pc) {
  RequestPtr req(new Request(paddr, size, 0, masterID,
                             reinterpret_cast<InstSeqNum>(handler), contextId));
  if (pc != 0) {
    req->setPC(pc);
  }
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

PacketPtr TDGPacketHandler::createStreamConfigPacket(Addr paddr,
                                                     MasterID masterID,
                                                     int contextId,
                                                     uint64_t data) {
  /**
   * ! Pure evil hack here.
   * ! Pass the data in pktData.
   */
  RequestPtr req(
      new Request(paddr, sizeof(uint64_t), 0, masterID, 1, contextId));
  MemCmd cmd(MemCmd::Command::StreamConfigReq);
  PacketPtr pkt = new Packet(req, cmd);
  uint8_t *pktData = new uint8_t[req->getSize()];
  *(reinterpret_cast<uint64_t *>(pktData)) = data;
  pkt->dataDynamic(pktData);
  return pkt;
}

void TDGPacketHandler::handleTDGPacketResponse(LLVMTraceCPU *cpu,
                                               PacketPtr pkt) {
  // Decode the handler information.
  auto handler = pkt->findNextSenderState<TDGPacketHandler>();
  assert(handler != NULL && "This is not a TDGPacket.");
  handler->handlePacketResponse(cpu, pkt);
}