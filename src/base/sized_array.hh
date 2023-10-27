#ifndef __BASE_SMALL_ARRAY_HH__
#define __BASE_SMALL_ARRAY_HH__

#include <array>
#include <cassert>
#include <cstring>

template <typename T, size_t capacity> class SizedArray {
public:
  SizedArray() : num_elements(0) {}

  using iterator = T *;

  T *begin() { return this->buffer.data(); }
  T *end() { return this->buffer.data() + this->num_elements; }
  T &operator[](size_t i) { return this->buffer[i]; }

  const T *begin() const { return this->buffer.data(); }
  const T *end() const { return this->buffer.data() + this->num_elements; }
  const T &operator[](size_t i) const { return this->buffer[i]; }

  void push_back(const T &v) {
    assert(this->num_elements < capacity);
    this->buffer[this->num_elements] = v;
    this->num_elements++;
  }

  bool hasOverlap(const SizedArray<T, capacity> &other) const {
    for (auto j : other) {
      if (this->contains(j)) {
        return true;
      }
    }
    return false;
  }
  bool contains(const T &j) const {
    for (auto i : *this) {
      if (i == j) {
        return true;
      }
    }
    return false;
  }

  bool empty() const { return this->num_elements == 0; }

  int32_t size() const { return this->num_elements; }

  void clear() { this->num_elements = 0; }

  std::array<T, capacity> buffer;
  int32_t num_elements;
};

#endif