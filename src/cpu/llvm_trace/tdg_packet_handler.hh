#ifndef __CPU_TDG_PACKET_HANDLER_H__
#define __CPU_TDG_PACKET_HANDLER_H__

#include "mem/packet.hh"

class LLVMTraceCPU;
class TDGPacketHandler {
public:
  /**
   * Handle a packet response.
   * Remember to release the packet at the end of this function.
   *
   * delete packet->req;
   * delete packet;
   */
  virtual void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) = 0;
  virtual ~TDGPacketHandler() {}
};

#endif