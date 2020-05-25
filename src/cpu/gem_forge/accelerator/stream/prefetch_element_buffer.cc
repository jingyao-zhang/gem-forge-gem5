
#include "prefetch_element_buffer.hh"

#include "stream.hh"
#include "stream_element.hh"

void PrefetchElementBuffer::addElement(StreamElement *element) {
  assert(!element->isStepped && "Insert stepped element into PEB.");
  assert(!element->isFirstUserDispatched() &&
         "Insert element with first user dispatched.");
  assert(element->stream->getStreamType() == ::LLVM::TDG::StreamInfo_Type_LD &&
         "Only load stream should be in PEB.");
  assert(!element->stream->getFloatManual() &&
         "FloatManual stream is alias-free and should not be added to PEB.");
  assert(element->isAddrReady && "Addr not ready element into PEB.");
  auto inserted = this->elements.emplace(element).second;
  assert(inserted && "Element already in PEB.");
}

void PrefetchElementBuffer::removeElement(StreamElement *element) {
  assert(this->elements.count(element) && "Element not in PEB.");
  this->elements.erase(element);
}

bool PrefetchElementBuffer::isHit(Addr vaddr, int size) const {
  for (auto element : this->elements) {
    if (element->addr >= vaddr + size ||
        element->addr + element->size <= vaddr) {
      continue;
    }
    element->isAddrAliased = true;
    return true;
  }
  return false;
}