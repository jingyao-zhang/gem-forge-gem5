#ifndef __CPU_DYN_INST_STREAM_HH__
#define __CPU_DYN_INST_STREAM_HH__

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "TDGInstruction.pb.h"
#include "llvm_insts.hh"
#include "queue_buffer.hh"

#include "proto/protoio.hh"

#include <list>
#include <utility>

namespace gem5 {

/**
 * Represent a instruction stream from a file.
 * The instructions are read by fetch stage, and are only released when
 * committed. Instructions must be committed in order.
 */
class DynamicInstructionStream {
public:
  using Buffer = QueueBuffer<DynamicInstructionStreamPacket>;
  using Iterator = Buffer::iterator;

  DynamicInstructionStream(std::shared_ptr<Buffer> _buffer);
  ~DynamicInstructionStream();

  DynamicInstructionStream(const DynamicInstructionStream &other) = delete;
  DynamicInstructionStream(DynamicInstructionStream &&other) = delete;
  DynamicInstructionStream &
  operator=(const DynamicInstructionStream &other) = delete;
  DynamicInstructionStream &
  operator=(DynamicInstructionStream &&other) = delete;

  /**
   * Parse more instructions from the file (currently at most 1000).
   * Returns the number of instructions parsed.
   *
   * NOTE: user should do some flow control based on the size(), otherwise the
   * memory usage will blow up.
   */

  size_t size() const { return this->buffer->getSize(); }
  bool empty() const { return this->size() == 0; }

  /**
   * Interface for fetch stage.
   */
  size_t fetchSize() const { return this->size() - this->fetchedSize; }
  bool fetchEmpty() const { return this->fetchSize() == 0; }
  LLVMDynamicInst *fetch();

  /**
   * Fetch the iterator.
   */
  Iterator fetchIter();

  /**
   * Interface for commit stage.
   */
  void commit(LLVMDynamicInst *inst);

  /**
   * Special case for out-of-order release.
   */
  void commit(Iterator instIter);

private:
  std::shared_ptr<Buffer> buffer;

  Iterator fetchPos;
  size_t fetchedSize;

  void release();
};

/**
 * A wrapper over the underneath DynamicInstructionStream.
 */
class DynamicInstructionStreamInterface {
public:
  /**
   * Fetch next instruction, nullptr if we reached the end of the stream.
   */
  virtual ~DynamicInstructionStreamInterface() {}
  virtual LLVMDynamicInst *fetch() = 0;
  virtual bool hasEnded() const = 0;
  virtual void commit(LLVMDynamicInst *inst) = 0;
};

/**
 * Implement a conditional ending the interface,
 * i.e. terminate the stream by a user-defined end token.
 * Note: The end token is consumed by the interface.
 */
class DynamicInstructionStreamInterfaceConditionalEnd
    : public DynamicInstructionStreamInterface {
public:
  using EndFunc = std::function<bool(LLVMDynamicInst *)>;
  using Iterator = DynamicInstructionStream::Iterator;
  DynamicInstructionStreamInterfaceConditionalEnd(
      DynamicInstructionStream *_stream, EndFunc _endFunc);

  ~DynamicInstructionStreamInterfaceConditionalEnd() override;

  LLVMDynamicInst *fetch() override;
  bool hasEnded() const override { return this->ended; }
  void commit(LLVMDynamicInst *inst) override;

private:
  DynamicInstructionStream *stream;
  EndFunc endFunc;

  Iterator headIter;
  LLVMDynamicInst *endToken;
  size_t fetchedSize;
  bool ended;
};

/**
 * Implment a fixed end strean interface.
 */
class DynamicInstructionStreamInterfaceFixedEnd
    : public DynamicInstructionStreamInterface {
public:
  using Iterator = DynamicInstructionStream::Iterator;
  DynamicInstructionStreamInterfaceFixedEnd(DynamicInstructionStream *_stream,
                                            Iterator _lhs, Iterator _rhs);

  ~DynamicInstructionStreamInterfaceFixedEnd() override;

  LLVMDynamicInst *fetch() override;
  bool hasEnded() const override { return this->fetchIter == this->rhs; }
  void commit(LLVMDynamicInst *inst) override;

private:
  DynamicInstructionStream *stream;
  Iterator lhs, rhs;

  Iterator fetchIter;
  size_t fetchedSize;
};

} // namespace gem5

#endif