#ifndef __CPU_GEM_FORGE_CORE_HH__
#define __CPU_GEM_FORGE_CORE_HH__

#include "llvm_insts.hh"

namespace gem5 {

/**
 * An interface to the execution pipeline.
 */
class GemForgeCore {
public:
  enum InstStatus {
    FETCHED,      // In FetchQueue.
    DECODED,      // Decoded.
    DISPATCHED,   // Dispatched to instQueue.
    BLOCKED,      // Blocked by memory, should not check ready unless unblock.
    READY,        // Ready to be issued.
    ISSUED,       // Issue to FU.
    FINISHED,     // Finished computing.
    COMMIT,       // Sent to commit stage.
    COMMITTING,   // Committing.
    COMMITTED,    // Committed.
    WRITEBACKING, // Writing back.
    WRITEBACKED,  // Write backed.
  };
  InstStatus getInflyInstStatus(LLVMDynamicInstId instId) const = 0;
  void updateInflyInstStatus(LLVMDynamicInstId instId,
                             InstStatus newStatus) = 0;
};

} // namespace gem5

#endif