#include "gem_forge_lsq_callback.hh"

#include "base/logging.hh"

std::string GemForgeLSQCallback::getTypeString() const {
  switch (this->getType()) {
  default:
    panic("Unknown GemForgeLSQCallback Type: %d.", this->getType());
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
    Case(LOAD);
    Case(STORE);
#undef Case
  }
}