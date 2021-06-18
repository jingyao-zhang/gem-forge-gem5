#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_NDC_PACKET_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_NDC_PACKET_HH__

#include "fifo_entry_idx.hh"
#include "stream_atomic_op.hh"

#include <memory>
#include <vector>

class StreamNDCPacket;
using StreamNDCPacketPtr = std::shared_ptr<StreamNDCPacket>;
using StreamNDCPacketWeakPtr = std::weak_ptr<StreamNDCPacket>;
using StreamNDCPacketVec = std::vector<StreamNDCPacketPtr>;

class Stream;

class StreamNDCPacket {
public:
  Stream *stream;
  FIFOEntryIdx entryIdx;
  Addr vaddr;
  Addr paddr;
  std::unique_ptr<StreamAtomicOp> atomicOp;
  StreamNDCPacket(Stream *_stream, const FIFOEntryIdx &_entryIdx, Addr _vaddr,
                  Addr _paddr)
      : stream(_stream), entryIdx(_entryIdx), vaddr(_vaddr), paddr(_paddr) {}

  /**
   * Used by NDCForward packet.
   */
  bool isForward = false;
  FIFOEntryIdx receiverEntryIdx;
  Addr receiverVAddr;
  Addr receiverPAddr;

  /**
   * Receiver of NDCForward packet.
   */
 StreamNDCPacketVec expectedForwardPackets;
};

#endif