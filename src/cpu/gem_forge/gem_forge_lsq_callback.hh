#ifndef __GEM_FORGE_LSQ_CALLBACK_HH
#define __GEM_FORGE_LSQ_CALLBACK_HH

#include "base/types.hh"

#include <array>
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
  virtual bool getAddrSize(Addr &addr, uint32_t &size) = 0;
  /**
   * Check if the load request has been issued to memory.
   */
  virtual bool isIssued() = 0;
  /**
   * Check if the value is loaded.
   */
  virtual bool isValueLoaded() = 0;
  virtual void RAWMisspeculate() = 0;
};

using GemForgeLQCallbackPtr = std::unique_ptr<GemForgeLQCallback>;

/**
 * TODO: Extend this.
 */
using GemForgeLQCallbackList = std::array<GemForgeLQCallbackPtr, 1>;

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