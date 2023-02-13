#include "LLCStreamSlice.hh"

#include "LLCStreamEngine.hh"

namespace gem5 {

LLCStreamSlice::LLCStreamSlice(Stream *_S, const DynStreamSliceId &_sliceId)
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

void LLCStreamSlice::responded(const ruby::DataBlock &loadBlock,
                               const ruby::DataBlock &storeBlock) {
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
    if (this->llcSE->myMachineType() == ruby::MachineType_Directory) {
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
  if (this->loadComputeValueSent) {
    panic("LoadComputeValue already sent %s.", this->getSliceId());
  }
  this->loadComputeValueSent = true;
}

void LLCStreamSlice::setProcessed() {
  assert(!this->processed && "AtomicOrUpdateSlice already processed.");
  this->processed = true;
}

const char *LLCStreamSlice::stateToString(State state) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (state) {
    Case(INITIALIZED);
    Case(ALLOCATED);
    Case(ISSUED);
    Case(RESPONDED);
    Case(FAULTED);
    Case(RELEASED);
#undef Case
  default:
    assert(false && "Invalid LLCStreamSlice::State.");
  }
}} // namespace gem5

