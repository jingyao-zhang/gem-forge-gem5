
#include "prefetch_element_buffer.hh"

void PrefetchElementBuffer::addElement(StreamElement *element) {
  auto inserted = this->elements.emplace(element).second;
  assert(inserted && "Element already in PEB.");
}

void PrefetchElementBuffer::removeElement(StreamElement *element) {
  assert(this->elements.count(element) && "Element not in PEB.");
  this->elements.erase(element);
}

bool PrefetchElementBuffer::isHit(Addr vaddr, int size) const { return false; }