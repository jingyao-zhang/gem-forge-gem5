#include "gem_forge_load_request.hh"

namespace Minor {

void GemForgeLoadRequest::checkIsComplete() {
  // If already complete, then done.
  if (this->isComplete()) {
    return;
  }
  // Check the LQ callback.
  bool completed = this->callback->isValueLoaded();
  if (completed) {
    this->setState(LSQRequest::Complete);
  }
}

} // namespace Minor