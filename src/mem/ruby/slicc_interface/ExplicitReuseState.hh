#ifndef __MEM_RUBY_SLICC_INTERFACE_EXPLICIT_REUSE_STATE_HH__
#define __MEM_RUBY_SLICC_INTERFACE_EXPLICIT_REUSE_STATE_HH__

#include <iostream>
#include <sstream>
#include <string>

namespace gem5 {
/**
 * Used to implemented explicit reuse in GemForge.
 */
struct ExplicitReuseState {

  using ReuseCount = int;

  static constexpr ReuseCount UnknownReuse = -1;
  ReuseCount total = UnknownReuse;
  ReuseCount current = UnknownReuse;

  void setTotal(ReuseCount total) { this->total = total; }
  void setCurrent(ReuseCount current) { this->current = current; }

  ReuseCount getTotal() const { return total; }
  ReuseCount getCurrent() const { return this->current; }

  bool isValid() const { return this->total != UnknownReuse; }
};

std::ostream &operator<<(std::ostream &os, const ExplicitReuseState &r);

std::string to_string(const ExplicitReuseState &r);
} // namespace gem5

#endif