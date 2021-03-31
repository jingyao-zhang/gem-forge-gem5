#include "stream_atomic_op.hh"

#include "stream.hh"

#include "debug/StreamAtomicOp.hh"
#define DEBUG_TYPE StreamAtomicOp
#include "stream_log.hh"

void StreamAtomicOp::operator()(uint8_t *p) {
  // Read in the final atomic operand from p to params.
  StreamValue operand;
  memcpy(operand.uint8Ptr(), p, this->size);
  this->params.back() = operand;
  auto result = this->storeFunc->invoke(params);

  if (this->loadFunc) {
    this->loadedValue = this->loadFunc->invoke(params);
  } else {
    // By default return the old value.
    this->loadedValue = operand;
  }

  // Check if we modified the memory.
  for (int i = 0; i < this->size; ++i) {
    if (operand[i] != result[i]) {
      this->memoryModified = true;
      break;
    }
  }

  S_DPRINTF(stream, "Entry (%lu, %lu): AtomicOp %lu -> %lu, Loaded? %d %lu.\n",
            entryIdx.streamId.streamInstance, entryIdx.entryIdx,
            operand.front(), result.front(), this->loadFunc != nullptr,
            this->loadedValue.front());

  memcpy(p, result.uint8Ptr(), this->size);
}