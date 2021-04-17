#ifndef __GEM_FORGE_IDEA_CACHE_HH__
#define __GEM_FORGE_IDEA_CACHE_HH__

#include "base/types.hh"

#include <list>
#include <unordered_map>

/**
 * This is an ideal cache:
 * 1. Fully associative.
 * 2. 1-byte line.
 * 3. LRU.
 */
class GemForgeIdeaCache {
public:
  GemForgeIdeaCache(int _size);

  /**
   * Access the data, update the cache.
   * @return number of missed bytes.
   */
  int access(Addr paddr, int size);

private:
  const int size;

  using LRUList = std::list<Addr>;
  using LRUListIter = LRUList::iterator;

  LRUList lru;
  std::unordered_map<Addr, LRUListIter> addrLRUMap;

  /**
   * Access single byte.
   */
  int accessByte(Addr paddr);
};

#endif