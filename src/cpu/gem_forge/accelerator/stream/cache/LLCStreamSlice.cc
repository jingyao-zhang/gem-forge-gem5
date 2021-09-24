#include "LLCStreamSlice.hh"

#include "LLCStreamEngine.hh"

LLCStreamSlice::LLCStreamSlice(const DynamicStreamSliceId &_sliceId)
    : sliceId(_sliceId) {}

void LLCStreamSlice::allocate(LLCStreamEngine *llcSE) {
  assert(this->state == State::INITIALIZED &&
         "Allocate from state other than INITIALIZED.");
  this->state = State::ALLOCATED;
  this->llcSE = llcSE;
}

void LLCStreamSlice::issue() {
  assert(this->state == State::ALLOCATED &&
         "Issue from state other than ALLOCATED.");
  this->state = State::ISSUED;
}

void LLCStreamSlice::responded(const DataBlock &loadBlock,
                               const DataBlock &storeBlock) {
  assert(this->state == State::ISSUED &&
         "Responded from state other than ISSUED.");
  this->state = State::RESPONDED;
  this->loadBlock = loadBlock;
  this->storeBlock = storeBlock;
}

void LLCStreamSlice::faulted() {
  assert(this->state == State::ALLOCATED &&
         "Fault from state other than ALLOCATED.");
  this->state = State::FAULTED;
}

void LLCStreamSlice::released() {
  assert((this->state == State::RESPONDED || this->state == State::FAULTED) &&
         "Fault from state other than RESPONDED or FAULTED.");
  this->state = State::RELEASED;
}

void LLCStreamSlice::setLoadComputeValueSent() {
  assert(!this->loadComputeValueSent && "LoadComputeValue already sent.");
  this->loadComputeValueSent = true;
}

void LLCStreamSlice::setProcessed() {
  assert(!this->processed && "AtomicOrUpdateSlice already processed.");
  this->processed = true;
}