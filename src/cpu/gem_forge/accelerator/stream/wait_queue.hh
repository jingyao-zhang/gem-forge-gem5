#ifndef GEM_FORGE_WAIT_QUEUE_HH
#define GEM_FORGE_WAIT_QUEUE_HH

#include <queue>
#include <unordered_map>

namespace gem5 {

class WaitQueue {
public:
  using TimeStamp = uint64_t;

  void push(int id, TimeStamp cycle) {
    if (!idToTimeStampMap.count(id)) {
      queue.push({id, cycle});
    }
    idToTimeStampMap[id] = cycle;
  }

  void removeStale(TimeStamp currentCycle, TimeStamp threshold) {
    while (!queue.empty() && queue.front().second + threshold < currentCycle) {
      idToTimeStampMap.erase(queue.front().first);
      queue.pop();
    }
  }

  bool checkIsHead(int id) {
    return !queue.empty() && queue.front().first == id;
  }

  void pop(int id) {
    assert(checkIsHead(id));
    queue.pop();
    idToTimeStampMap.erase(id);
  }

private:
  std::queue<std::pair<int, int>> queue;
  std::unordered_map<int, int> idToTimeStampMap;
};
} // namespace gem5

#endif