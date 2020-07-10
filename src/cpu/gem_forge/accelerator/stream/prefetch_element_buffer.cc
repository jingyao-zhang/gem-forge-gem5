
#include "prefetch_element_buffer.hh"

#include "stream.hh"
#include "stream_element.hh"

#include "debug/PrefetchElementBuffer.hh"
#define DEBUG_TYPE PrefetchElementBuffer
#include "stream_log.hh"

void PrefetchElementBuffer::addElement(StreamElement *element) {
  assert(!element->isFirstUserDispatched() &&
         "Insert element with first user dispatched.");
  assert(element->stream->isLoadStream() &&
         "Only load stream should be in PEB.");
  assert(!element->stream->getFloatManual() &&
         "FloatManual stream is alias-free and should not be added to PEB.");
  assert(element->isAddrReady && "Addr not ready element into PEB.");
  auto inserted = this->elements.emplace(element).second;
  assert(inserted && "Element already in PEB.");
  S_ELEMENT_DPRINTF(element, "Add to PEB.\n");
}

void PrefetchElementBuffer::removeElement(StreamElement *element) {
  S_ELEMENT_DPRINTF(element, "Remove from PEB.\n");
  if (!this->elements.count(element)) {
    S_ELEMENT_PANIC(element, "Element not in PEB.");
  }
  this->elements.erase(element);
}

StreamElement *PrefetchElementBuffer::isHit(Addr vaddr, int size) const {
  for (auto element : this->elements) {
    S_ELEMENT_DPRINTF(element, "PEB check (%#x, +%d) against (%#x, +%d).\n",
                      vaddr, size, element->addr, element->size);
    if (element->addr >= vaddr + size ||
        element->addr + element->size <= vaddr) {
      continue;
    }
    return element;
  }
  return nullptr;
}