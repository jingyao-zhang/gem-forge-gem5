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

#include "proto/protoio.hh"

/**
 * Represent a instruction stream from a file.
 * This file simply parses the
 */
class DynamicInstructionStreamDispatcher {
public:
  DynamicInstructionStreamDispatcher(const std::string &_fn);
  ~DynamicInstructionStreamDispatcher();

  DynamicInstructionStreamDispatcher(
      const DynamicInstructionStreamDispatcher &other) = delete;
  DynamicInstructionStreamDispatcher(
      DynamicInstructionStreamDispatcher &&other) = delete;
  DynamicInstructionStreamDispatcher &
  operator=(const DynamicInstructionStreamDispatcher &other) = delete;
  DynamicInstructionStreamDispatcher &
  operator=(DynamicInstructionStreamDispatcher &&other) = delete;

  using Buffer = QueueBuffer<DynamicInstructionStreamPacket>;

  std::shared_ptr<Buffer> getMainBuffer() { return this->mainBuffer; }

  void parse();

  /**
   * Get the static info at the header of the stream.
   */
  const LLVM::TDG::StaticInformation &getStaticInfo() const {
    return this->staticInfo;
  }

private:
  std::string fn;
  ProtoInputStream *input;
  LLVM::TDG::StaticInformation staticInfo;

  std::shared_ptr<Buffer> mainBuffer;
  std::shared_ptr<Buffer> currentBuffer;
};

#endif