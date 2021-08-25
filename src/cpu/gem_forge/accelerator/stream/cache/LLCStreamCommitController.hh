#ifndef __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_COMMIT_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_COMMIT_CONTROLLER_HH__

#include "LLCStreamEngine.hh"

#include <list>

class LLCStreamCommitController {
public:
  LLCStreamCommitController(LLCStreamEngine *_se);

  void registerStream(LLCDynamicStreamPtr dynS);
  void deregisterStream(LLCDynamicStreamPtr dynS);

  bool hasStreamToCommit() const { return !this->streams.empty(); }

  void commit();

private:
  LLCStreamEngine *se;

  std::list<LLCDynamicStreamPtr> streams;

  int curRemoteBank() const { return this->se->curRemoteBank(); }
  const char *curRemoteMachineType() const {
    return this->se->curRemoteMachineType();
  }

  /**
   * Try to commit an element for the stream.
   * @return whether we have committed at least one element.
   */
  bool commitStream(LLCDynamicStreamPtr dynS, bool &migrated);
};

#endif