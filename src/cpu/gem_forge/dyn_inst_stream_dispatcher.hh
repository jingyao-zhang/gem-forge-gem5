#ifndef __CPU_DYN_INST_STREAM_DISPATCHER_HH__
#define __CPU_DYN_INST_STREAM_DISPATCHER_HH__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "TDGInstruction.pb.h"
#include "llvm_insts.hh"
#include "queue_buffer.hh"
#include "region_table.hh"

#include "proto/protoio.hh"

namespace gem5 {

/**
 * Represent a instruction stream from a file.
 * This file simply parses the
 */
class DynamicInstructionStreamDispatcher {
public:
  DynamicInstructionStreamDispatcher(const std::string &_fn,
                                     bool _enableADFA = false);
  ~DynamicInstructionStreamDispatcher();

  DynamicInstructionStreamDispatcher(
      const DynamicInstructionStreamDispatcher &other) = delete;
  DynamicInstructionStreamDispatcher(
      DynamicInstructionStreamDispatcher &&other) = delete;
  DynamicInstructionStreamDispatcher &
  operator=(const DynamicInstructionStreamDispatcher &other) = delete;
  DynamicInstructionStreamDispatcher &
  operator=(DynamicInstructionStreamDispatcher &&other) = delete;

  using Packet = DynamicInstructionStreamPacket;
  using Buffer = QueueBuffer<Packet>;

  std::shared_ptr<Buffer> getMainBuffer() { return this->mainBuffer; }

  void parse();
  void dispatch(Packet *packet);

  /**
   * Get the static info at the header of the stream.
   */
  const LLVM::TDG::StaticInformation &getStaticInfo() const {
    return this->staticInfo;
  }

  const RegionTable &getRegionTable() const { return *this->regionTable; }

private:
  std::string fn;
  bool enableADFA;
  ProtoInputStream *input;
  LLVM::TDG::StaticInformation staticInfo;
  RegionTable *regionTable;

  std::shared_ptr<Buffer> mainBuffer;
  std::shared_ptr<Buffer> currentBuffer;

  /**
   * Used for ADFA dispatcher.
   */
  bool inContinuousRegion;

  void dispatchADFA(Packet *packet);
};

} // namespace gem5

#endif