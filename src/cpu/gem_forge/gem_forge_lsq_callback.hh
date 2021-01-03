#ifndef __GEM_FORGE_LSQ_CALLBACK_HH
#define __GEM_FORGE_LSQ_CALLBACK_HH

#include "base/types.hh"

#include <array>
#include <iomanip>
#include <iostream>
#include <memory>

/**
 * This is used as the interface to let the core LSQ query about
 * information from the SE.
 */

struct GemForgeLSQCallback {
public:
  enum Type {
    LOAD,
    STORE,
  };
  virtual Type getType() const = 0;
  std::string getTypeString() const;
  /**
   * Get the address and size of this lsq entry.
   * @return true if the address is ready.
   */
  virtual bool getAddrSize(Addr &addr, uint32_t &size) const = 0;
  bool isAddrReady() const {
    Addr addr;
    uint32_t size;
    return this->getAddrSize(addr, size);
  }
  Addr getAddr() const {
    Addr addr;
    uint32_t size;
    assert(this->getAddrSize(addr, size));
    return addr;
  }
  uint32_t getSize() const {
    Addr addr;
    uint32_t size;
    assert(this->getAddrSize(addr, size));
    return size;
  }

  /**
   * Check if the request has non core user, i.e.
   * deps that is not tracked by normal register read/write.
   * This basically means that if this is flushed, we have to notify the SE.
   */
  virtual bool hasNonCoreDependent() const = 0;
  /**
   * Check if the request has been issued to memory by the SE.
   */
  virtual bool isIssued() const = 0;
  /**
   * Check if the value is loaded.
   */
  virtual bool isValueReady() const = 0;
  /**
   * Get a pointer to the value.
   */
  virtual const uint8_t *getValue() const {
    assert(false && "GetValue not impelmented.");
  }

  virtual void RAWMisspeculate() = 0;

  /**
   * Whether the compiler feels confident that this access should
   * be alias free.
   */
  virtual bool bypassAliasCheck() const { return false; }

  virtual std::ostream &format(std::ostream &os) const {
    Addr addr = 0;
    uint32_t size = 0;
    bool addrReady = this->isAddrReady();
    return os << "[" << this->getTypeString() << " AddrReady " << addrReady
              << ", Issued " << this->isIssued() << ", 0x" << std::hex << addr
              << ", +" << size << ']';
  }
  friend std::ostream &operator<<(std::ostream &os,
                                  const GemForgeLSQCallback &cb) {
    return cb.format(os);
  }
};

using GemForgeLSQCallbackPtr = std::unique_ptr<GemForgeLSQCallback>;
using GemForgeLSQCallbackList = std::array<GemForgeLSQCallbackPtr, 4>;

struct GemForgeLQCallback : public GemForgeLSQCallback {
  Type getType() const override { return Type::LOAD; }
};

using GemForgeLQCallbackPtr = std::unique_ptr<GemForgeLQCallback>;
using GemForgeLQCallbackList = std::array<GemForgeLQCallbackPtr, 4>;

struct GemForgeSQCallback : public GemForgeLSQCallback {
  Type getType() const override { return Type::STORE; }
};

struct GemForgeSQDeprecatedCallback {
  /**
   * * Get the address and size of this lsq entry.
   * @return true if the address is ready.
   */
  virtual bool getAddrSize(Addr &addr, uint32_t &size) = 0;
  virtual void writeback() = 0;
  virtual bool isWritebacked() = 0;
  virtual void writebacked() = 0;
};

#endif