#include "gem_forge_load_request.hh"

#include "debug/MinorGemForgeLoadRequest.hh"

#define INST_DPRINTF(inst, format, args...)                                    \
  DPRINTF(MinorGemForgeLoadRequest, "[%s]: " format, *(inst), ##args)

namespace Minor {

void GemForgeLoadRequest::checkIsComplete() {
  // If already complete, then done.
  if (this->isComplete()) {
    return;
  }
  // Check the LQ callback.
  bool completed = this->callback->isValueReady();
  if (completed) {
    INST_DPRINTF(this->inst, "checkIsComplete() succeed.\n");
    this->setState(LSQRequest::Complete);
  }
}

void GemForgeLoadRequest::markDiscarded() {
  assert(!this->discarded && "Mark discarded twice.");
  this->discarded = true;
  if (!this->isComplete()) {
    // Mark myself completed.
    this->setState(LSQRequest::Complete);
  }
}

} // namespace Minor