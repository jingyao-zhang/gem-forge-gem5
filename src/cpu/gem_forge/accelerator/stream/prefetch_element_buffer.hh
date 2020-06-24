#ifndef __GEM_FORGE_STREAM_PREFETCH_ELEMENT_BUFFER_HH__
#define __GEM_FORGE_STREAM_PREFETCH_ELEMENT_BUFFER_HH__

#include "stream_element.hh"

#include <unordered_set>

/**
 * This data struction holds the stream element that's in prefetch state.
 * These elements should be
 * isAddrReady = true
 * isFirstUserDispatched = false
 * isStepped = false
 * isLoad = true
 *
 * There is no order between these elements.
 * If the compiler can be sure that this stream will not be aliased, then
 * the StreamEngine can skip PEB.
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
   * @return the first hit element.
   */
  StreamElement *isHit(Addr vaddr, int size) const;

  std::unordered_set<StreamElement *> elements;
};

#endif