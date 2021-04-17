#include "gem_forge_idea_cache.hh"

GemForgeIdeaCache::GemForgeIdeaCache(int _size) : size(_size) {}

int GemForgeIdeaCache::access(Addr paddr, int size) {
  int missedBytes = 0;
  for (int i = 0; i < size; ++i) {
    missedBytes += this->accessByte(paddr + i);
  }
  return missedBytes;
}

int GemForgeIdeaCache::accessByte(Addr paddr) {
  auto mapIter = this->addrLRUMap.find(paddr);
  if (mapIter == this->addrLRUMap.end()) {
    // This is a miss.
    this->lru.push_back(paddr);
    auto lruIter = std::prev(this->lru.end());
    this->addrLRUMap.emplace(paddr, lruIter);
    if (this->lru.size() > this->size) {
      this->addrLRUMap.erase(this->lru.front());
      this->lru.pop_front();
    }
    return 1;
  } else {
    // This is a hit. Move to LRU.
    auto lruIter = mapIter->second;
    this->lru.splice(this->lru.end(), this->lru, lruIter);
    auto newLRUIter = std::prev(this->lru.end());
    mapIter->second = newLRUIter;
    return 0;
  }
}