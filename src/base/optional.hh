#ifndef __GEM_FORGE_OPTIONAL_HH__
#define __GEM_FORGE_OPTIONAL_HH__

template <typename T> struct Optional {
  T _value;
  bool _valid = false;

  void set(const T &value) {
    _value = value;
    _valid = true;
  }

  void reset() { _valid = false; }

  bool has_value() const { return _valid; }

  const T &value() const { return _value; }
};

#endif