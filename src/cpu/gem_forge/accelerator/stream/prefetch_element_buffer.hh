#ifndef __GEM_FORGE_STREAM_PREFETCH_ELEMENT_BUFFER_HH__
#define __GEM_FORGE_STREAM_PREFETCH_ELEMENT_BUFFER_HH__

#include "stream_element.hh"

#include <unordered_set>

/**
 * This data struction holds the stream element that's in prefetch state.
 * There is no order between these elements.
 */
class PrefetchElementBuffer {
public:
  PrefetchElementBuffer() {}

  void addElement(StreamElement *element);
  void removeElement(StreamElement *element);
  bool contains(StreamElement *element) const {
    return this->elements.count(element) != 0;
  }

  /**
   * Check if there is store hit in PEB.
   */
  bool isHit(Addr vaddr, int size) const;

private:
  std::unordered_set<StreamElement *> elements;
};

#endif