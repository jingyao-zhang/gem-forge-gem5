
#ifndef __CPU_GEM_FORGE_QUEUE_BUFFER_HH__
#define __CPU_GEM_FORGE_QUEUE_BUFFER_HH__

#include <list>
#include <utility>

namespace gem5 {

/**
 * This class represents a buffer, where elements are allocated and deallocated
 * in a FIFO order.
 * The "deallocated" elements are reused for later allocation.
 */
template <typename T> class QueueBuffer {
public:
  explicit QueueBuffer() : size(0) {
    this->list.push_back(new T());
    this->_end = this->list.begin();
  }

  ~QueueBuffer() {
    for (auto &allocated : this->list) {
      delete allocated;
    }
    this->list.clear();
  }

  // For simplicity, no copy or move.
  QueueBuffer(const QueueBuffer &Other) = delete;
  QueueBuffer(QueueBuffer &&Other) = delete;
  QueueBuffer &operator=(const QueueBuffer &Other) = delete;
  QueueBuffer &operator=(QueueBuffer &&Other) = delete;

  using container = std::list<T *>;
  using iterator = typename container::iterator;

  iterator begin() { return this->list.begin(); }
  T &front() { return *(this->list.front()); }
  iterator end() { return this->_end; }

  size_t getSize() const { return this->size; }
  bool empty() const { return this->size == 0; }

  void release_front(const T *allocated) {
    if (this->empty()) {
      panic("Release from empty queue buffer.");
    }

    // Move the allocated one to the back for reuse.
    auto released = this->list.front();
    if (allocated != released) {
      panic("Release not in order.");
    }
    this->list.pop_front();
    this->list.push_back(released);
    this->size--;
  }

  /**
   * Get an allocated element, but without moving _end iterator.
   * If followed by alloc_back, then this element is allocated.
   * If followed by steal_back, then this element is not managed by the queue
   * buffer any more.
   */
  T *peek_back() { return *this->_end; }

  T *alloc_back() {
    auto allocated = *this->_end;
    this->_end++;
    this->size++;
    this->expand();
    return allocated;
  }

  T *steal_back() {
    auto stolen = *this->_end;
    this->_end = this->list.erase(this->_end);
    this->expand();
    return stolen;
  }

  /**
   * Push a newed element to the queue buffer.
   * * The element must be newed and the buffer has the ownership.
   */
  void push_back(T *element) {
    this->list.insert(this->_end, element);
    this->size++;
  }

private:
  std::list<T *> list;
  iterator _end;
  size_t size;

  void expand() {
    if (this->_end == this->list.end()) {
      // We ran out of space.
      // Really allocate one element.
      this->_end = this->list.insert(this->_end, new T());
    }
    if (this->_end == this->list.end()) {
      panic("Run out of space for allocation.");
    }
  }
};

} // namespace gem5

#endif