#ifndef __GEM_FORGE_LSQ_CALLBACK_HH
#define __GEM_FORGE_LSQ_CALLBACK_HH

#include "base/types.hh"

#include <array>
#include <iomanip>
#include <iostream>
#include <memory>

/**
 * This LSQ is designed to be abstract as the entries may not be a simple
 * instruction. For example, the stream engine may use this to handle aliasing.
 * It only knows about the interface/callback.
 */

struct GemForgeLQCallback {
  /**
   * Get the address and size of this lsq entry.
   * @return true if the address is ready.
   */
  virtual bool getAddrSize(Addr &addr, uint32_t &size) const = 0;
  /**
   * Check if the load request has been issued to memory.
   */
  virtual bool isIssued() const = 0;
  /**
   * Check if the value is loaded.
   */
  virtual bool isValueLoaded() = 0;

  virtual void RAWMisspeculate() = 0;

  /**
   * Whether the compiler feels confident that this access should
   * be alias free.
   */
  virtual bool bypassAliasCheck() const { return false; }

  virtual std::ostream &format(std::ostream &os) const {
    Addr addr = 0;
    uint32_t size = 0;
    bool ready = this->getAddrSize(addr, size);
    return os << "[Ready " << ready << ", Issued " << this->isIssued() << ", 0x"
              << std::hex << addr << ", +" << size << ']';
  }
  friend std::ostream &operator<<(std::ostream &os,
                                  const GemForgeLQCallback &cb) {
    return cb.format(os);
  }
};

using GemForgeLQCallbackPtr = std::unique_ptr<GemForgeLQCallback>;

/**
 * TODO: Extend this.
 */
using GemForgeLQCallbackList = std::array<GemForgeLQCallbackPtr, 4>;

struct GemForgeSQCallback {
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