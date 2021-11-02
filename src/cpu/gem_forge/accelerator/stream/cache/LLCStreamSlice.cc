#include "LLCStreamSlice.hh"

#include "LLCStreamEngine.hh"

LLCStreamSlice::LLCStreamSlice(Stream *_S, const DynamicStreamSliceId &_sliceId)
    : S(_S), sliceId(_sliceId) {}

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
  this->issuedCycle = this->llcSE->curCycle();
}

void LLCStreamSlice::responded(const DataBlock &loadBlock,
                               const DataBlock &storeBlock) {
  assert(this->state == State::ISSUED &&
         "Responded from state other than ISSUED.");
  this->state = State::RESPONDED;
  this->loadBlock = loadBlock;
  this->storeBlock = storeBlock;
  this->respondedCycle = this->llcSE->curCycle();
  /**
   * So far this only works for DirectStream.
   * ReqLatency for IndirectStream is recorded in LLCStreamEngine.
   */
  if (S->isDirectMemStream()) {
    auto &statistic = this->S->statistic;
    if (this->llcSE->myMachineType() == MachineType_Directory) {
      statistic.memReqLat.sample(this->respondedCycle - this->issuedCycle);
    } else {
      statistic.llcReqLat.sample(this->respondedCycle - this->issuedCycle);
    }
  }
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