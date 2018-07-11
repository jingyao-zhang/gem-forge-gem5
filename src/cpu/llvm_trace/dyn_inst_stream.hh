#ifndef __CPU_DYN_INST_STREAM_HH__
#define __CPU_DYN_INST_STREAM_HH__

#include "base/misc.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse llvm instructions."
#endif

#include "TDGInstruction.pb.h"
#include "llvm_accelerator.hh"
#include "llvm_insts.hh"

#include "proto/protoio.hh"

#include <list>

/**
 * This class represents a buffer, where elements are allocated and deallocated
 * in a FIFO order.
 * The "deallocated" elements are reused for later allocation.
 */
template <typename T> class QueueBuffer {
public:
  explicit QueueBuffer() : size(0) { this->_end = this->list.end(); }

  ~QueueBuffer() {
    for (auto &allocated : this->list) {
      delete allocated;
    }
  }

  // For simplicity, no copy or move.
  QueueBuffer(const QueueBuffer &Other) = delete;
  QueueBuffer(QueueBuffer &&Other) = delete;
  QueueBuffer &operator=(const QueueBuffer &Other) = delete;
  QueueBuffer &operator=(QueueBuffer &&Other) = delete;

  using container = std::list<T *>;
  using iterator = typename container::iterator;

  iterator begin() { return this->list.begin(); }
  iterator end() { return this->_end; }

  bool empty() const { return this->size == 0; }

  void release_front() {
    if (this->empty()) {
      panic("Pop from empty circular list.");
    }
    // Move the allocated one to the back for reuse.
    auto popped = this->list.front();
    this->list.pop_front();
    this->list.push_back(popped);
    this->size--;
  }

  T *alloc_back() {
    if (this->_end == this->list.end()) {
      // We ran out of space.
      // Really allocate one element.
      this->_end = this->list.insert(this->_end, new T());
    }
    if (this->_end == this->list.end()) {
      panic("Run out of space for allocation.");
    }
    auto allocated = *this->_end;
    this->_end++;
    this->size++;
    return allocated;
  }

private:
  std::list<T *> list;
  iterator _end;
  size_t size;
};

/**
 * Represent a instruction stream from a file.
 * The instructions are read by fetch stage, and are only released when
 * committed. Instructions must be committed in order.
 */
class DynamicInstructionStream {
public:
  DynamicInstructionStream(const std::string &_fn);
  DynamicInstructionStream(const DynamicInstructionStream &other) = delete;
  DynamicInstructionStream(DynamicInstructionStream &&other) = delete;
  DynamicInstructionStream &
  operator=(const DynamicInstructionStream &other) = delete;
  DynamicInstructionStream &
  operator=(DynamicInstructionStream &&other) = delete;

  /**
   * Parse more instructions from the file.
   * Returns the number of instructions parsed.
   */
  size_t parse();

  size_t size() const { return this->insts.size(); }
  bool empty() const { return this->size() == 0; }

  /**
   * Interface for fetch stage.
   */
  size_t fetchSize() const { return this->_fetchSize; }
  bool fetchEmpty() const { return this->fetchSize() == 0; }
  LLVMDynamicInst *fetch();

  /**
   * Interface for commit stage.
   */
  void commit(LLVMDynamicInst *inst);

private:
  std::string fn;
  ProtoInputStream *input;
  QueueBuffer<LLVM::TDG::TDGInstruction> buffer;

  std::list<LLVMDynamicInst *>::iterator fetchPos;
  size_t _fetchSize;

  std::list<LLVMDynamicInst *> insts;
};

#endif