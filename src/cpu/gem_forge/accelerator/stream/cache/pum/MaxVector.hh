#ifndef __CPU_GEM_FORGE_MAX_VECTOR_HH__
#define __CPU_GEM_FORGE_MAX_VECTOR_HH__

#include <array>

#include "base/logging.hh"

namespace gem5 {

/**
 * ! Not sure this is correct.
 * ! Only use it for simple data structures (no complicate resource management).
 */

template <typename T, size_t N> class MaxVector {
private:
  char _data[sizeof(T) * N];

  size_t count = 0;

public:
  typedef T value_type;
  typedef value_type *pointer;
  typedef const value_type *const_pointer;
  typedef value_type &reference;
  typedef const value_type &const_reference;
  typedef value_type *iterator;
  typedef const value_type *const_iterator;
  typedef std::size_t size_type;
  typedef std::ptrdiff_t difference_type;
  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

  MaxVector() : count(0) {}

  iterator begin() noexcept { return iterator(data()); }

  const_iterator begin() const noexcept { return const_iterator(data()); }

  iterator end() noexcept { return iterator(data() + count); }

  const_iterator end() const noexcept { return const_iterator(data() + count); }

  reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }

  const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

  const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }

  const_iterator cbegin() const noexcept { return const_iterator(data()); }

  const_iterator cend() const noexcept {
    return const_iterator(data() + count);
  }

  const_reverse_iterator crbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  const_reverse_iterator crend() const noexcept {
    return const_reverse_iterator(begin());
  }

  reference operator[](size_type n) noexcept { return *(data() + n); }

  constexpr const_reference operator[](size_type n) const noexcept {
    return *(data() + n);
  }

  reference at(size_type n) {
    if (n >= count) {
      panic("MaxVector::at: n (which is %zu) >= count (which is %zu)", n,
            count);
    }
    return *(data() + n);
  }

  const_reference at(size_type n) const {
    if (n >= count) {
      panic("MaxVector::at: n (which is %zu) >= count (which is %zu)", n,
            count);
    }
    return *(data() + n);
  }

  reference front() { return at(0); }
  const_reference front() const { return at(0); }

  reference back() { return at(count - 1); }
  const_reference back() const { return at(count - 1); }

  bool empty() const { return count == 0; }
  size_t size() const { return count; }

  pointer data() noexcept { return reinterpret_cast<pointer>(_data); };

  const_pointer data() const noexcept {
    return reinterpret_cast<const_pointer>(_data);
  }

  template <typename... Args> void emplace_back(Args &&...args) {
    if (count == N) {
      panic("MaxVector overflow.");
    }
    T *p = data() + count;
    new (p) T(std::forward<Args>(args)...);
    count++;
  }

  iterator insert(iterator pos, const T &value) {
    if (count == N) {
      panic("MaxVector insert overflow.");
    }
    if (pos > end() || pos < begin()) {
      panic("MaxVector insert invalid pos.");
    }
    for (auto iter = end(); iter > pos; --iter) {
      new (iter) T(std::move(*(iter - 1)));
    }
    new (pos) T(value);
    count++;
    return pos;
  }

  iterator insert(iterator pos, const_iterator first, const_iterator last) {
    auto insert_count = last - first;
    if (count + insert_count > N) {
      panic("MaxVector insert overflow.");
    }
    if (pos > end() || pos < begin()) {
      panic("MaxVector insert invalid pos.");
    }
    for (auto iter = end(); iter > pos; --iter) {
      new (iter + insert_count - 1) T(std::move(*(iter - 1)));
    }
    for (auto iter = pos, iter2 = first; iter2 < last; ++iter, ++iter2) {
      new (iter) T(*iter2);
    }
    count += insert_count;
    return pos;
  }

  void push_back(const T &t) { return emplace_back(t); }
  void push_back(T &&t) { return emplace_back(std::move(t)); }
  void pop_back() {
    if (count == 0) {
      panic("MaxVector underflow.");
    }
    count--;
    data()[count].~T();
  }

  void clear() { count = 0; }
};

} // namespace gem5

#endif