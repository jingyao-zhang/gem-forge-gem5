#include "dyn_inst_stream.hh"

// For the DPRINTF function.
#include "base/trace.hh"
#include "debug/LLVMTraceCPU.hh"

namespace gem5 {

DynamicInstructionStream::DynamicInstructionStream(
    std::shared_ptr<Buffer> _buffer)
    : buffer(_buffer) {
  this->fetchedSize = 0;
  assert(!this->buffer->empty() && "Empty buffer to create a stream.");
  this->fetchPos = this->buffer->begin();
}

DynamicInstructionStream::~DynamicInstructionStream() {}

LLVMDynamicInst *DynamicInstructionStream::fetch() {
  if (this->fetchEmpty()) {
    panic("Fetch from empty instruction stream.");
  }
  auto fetched = (*this->fetchPos)->inst;
  this->fetchPos++;
  this->fetchedSize++;
  return fetched;
}

DynamicInstructionStream::Iterator DynamicInstructionStream::fetchIter() {
  if (this->fetchEmpty()) {
    panic("Fetch from empty instruction stream.");
  }
  auto fetched = this->fetchPos;
  this->fetchPos++;
  this->fetchedSize++;
  return fetched;
}

void DynamicInstructionStream::commit(LLVMDynamicInst *inst) {
  if (this->empty()) {
    panic("Commit called for empty instruction stream.");
  }
  if (this->buffer->front().inst != inst) {
    panic("Commit called out of order.");
  }
  if (this->buffer->front().released) {
    panic("Double commit an instruction.");
  }
  this->buffer->front().released = true;
  this->release();
}

void DynamicInstructionStream::commit(Iterator instIter) {
  if (this->empty()) {
    panic("Commit called for empty instruction stream.");
  }
  (*instIter)->released = true;
  this->release();
}

void DynamicInstructionStream::release() {
  while (!this->empty() && this->buffer->front().released) {
    delete this->buffer->front().inst;
    this->buffer->front().inst = nullptr;
    this->buffer->release_front(*this->buffer->begin());
    this->fetchedSize--;
  }
}

DynamicInstructionStreamInterfaceConditionalEnd::
    DynamicInstructionStreamInterfaceConditionalEnd(
        DynamicInstructionStream *_stream, EndFunc _endFunc)
    : stream(_stream), endFunc(_endFunc), endToken(nullptr), fetchedSize(0),
      ended(false) {}

DynamicInstructionStreamInterfaceConditionalEnd::
    ~DynamicInstructionStreamInterfaceConditionalEnd() {
  // Remember to commit the endToken if there is one.
  if (this->endToken != nullptr) {
    this->commit(this->endToken);
    this->endToken = nullptr;
  }
  if (this->fetchedSize != 0) {
    panic("Not all fetched instructions are committed before releasing this "
          "conditional end stream interface.");
  }
}

LLVMDynamicInst *DynamicInstructionStreamInterfaceConditionalEnd::fetch() {
  if (this->ended) {
    return nullptr;
  }
  if (this->stream->fetchEmpty()) {
    // We have reached the end of the underneath stream.
    this->ended = true;
    return nullptr;
  }
  auto Iter = this->stream->fetchIter();
  if (this->fetchedSize == 0) {
    hack("Updated the headIter to Iter.\n");
    this->headIter = Iter;
  }
  this->fetchedSize++;
  // Check for endToken.
  if (this->endFunc((*Iter)->inst)) {
    if (this->endToken != nullptr) {
      panic("We have already fetched the end token.");
    }
    this->endToken = (*Iter)->inst;
    this->ended = true;
    // We have reached the end.
    return nullptr;
  }
  return (*Iter)->inst;
}

void DynamicInstructionStreamInterfaceConditionalEnd::commit(
    LLVMDynamicInst *inst) {
  if (this->fetchedSize == 0) {
    panic("Try to commit when we have fetched nothing.");
  }
  if ((*this->headIter)->inst != inst) {
    panic("Instruction %lu is not committed in order.",
          (*this->headIter)->inst->getId());
  }
  auto committedIter = this->headIter;
  ++this->headIter;
  this->stream->commit(committedIter);
  --this->fetchedSize;
}

DynamicInstructionStreamInterfaceFixedEnd::
    DynamicInstructionStreamInterfaceFixedEnd(DynamicInstructionStream *_stream,
                                              Iterator _lhs, Iterator _rhs)
    : stream(_stream), lhs(_lhs), rhs(_rhs), fetchIter(_lhs), fetchedSize(0) {}

DynamicInstructionStreamInterfaceFixedEnd::
    ~DynamicInstructionStreamInterfaceFixedEnd() {
  if (this->fetchedSize != 0) {
    panic("StreamInterfaceFixed released when there are still committed "
          "instructions.");
  }
}

LLVMDynamicInst *DynamicInstructionStreamInterfaceFixedEnd::fetch() {
  if (this->fetchIter == this->rhs) {
    return nullptr;
  }

  auto inst = (*this->fetchIter)->inst;
  this->fetchedSize++;
  ++this->fetchIter;

  return inst;
}

void DynamicInstructionStreamInterfaceFixedEnd::commit(LLVMDynamicInst *inst) {
  if (this->fetchedSize == 0) {
    panic("Try to commit when we have fetched nothing.");
  }
  if ((*this->lhs)->inst != inst) {
    panic("Instruction is not committed in order.");
  }
  auto committedIter = this->lhs;
  ++this->lhs;
  this->stream->commit(committedIter);
  --this->fetchedSize;
}} // namespace gem5

