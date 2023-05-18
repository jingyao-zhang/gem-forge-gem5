#ifndef __CPU_GEM_FORGE_CALLBACK_LIST_HH__
#define __CPU_GEM_FORGE_CALLBACK_LIST_HH__

#include <list>
#include <string>

namespace gem5 {

template <typename Callback> class CallbackList {
public:
  bool empty() const { return this->callbacks.empty(); }

  void registerCallback(Callback callback) {
    this->callbacks.emplace_back(std::move(callback));
  }

  template <typename... Args> void invokeCallback(Args... args) {
    auto iter = this->callbacks.begin();
    auto end = this->callbacks.end();
    while (iter != end) {
      auto &callback = *iter;
      if (callback(args...)) {
        // We are done.
        iter = this->callbacks.erase(iter);
      } else {
        ++iter;
      }
    }
  }

  std::list<Callback> callbacks;
};

} // namespace gem5

#endif