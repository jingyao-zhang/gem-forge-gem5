#ifndef __CPU_TDG_PACKET_HANDLER_H__
#define __CPU_TDG_PACKET_HANDLER_H__

#include "mem/packet.hh"

class LLVMTraceCPU;

/**
 * Drived from SenderState so that it's able to distinguish a TDGPacket
 * from other normal packet.
 */
class TDGPacketHandler : public Packet::SenderState {
public:
  /**
   * Handle a packet response.
   * Remember to release the packet at the end of this function.
   *
   * delete packet;
   */
  virtual void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) = 0;
  virtual ~TDGPacketHandler() {}
  static PacketPtr createTDGPacket(Addr paddr, int size,
                                   TDGPacketHandler *handler, uint8_t *data,
                                   MasterID masterID, int contextId, Addr pc);
  static PacketPtr createStreamConfigPacket(Addr paddr, MasterID masterID,
                                            int contextId);
  static void handleTDGPacketResponse(LLVMTraceCPU *cpu, PacketPtr pkt);
};

#endif