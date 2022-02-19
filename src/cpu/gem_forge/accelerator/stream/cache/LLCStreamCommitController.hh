#ifndef __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_COMMIT_CONTROLLER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_LLC_STREAM_COMMIT_CONTROLLER_HH__

#include "LLCStreamEngine.hh"

#include <list>

class LLCStreamCommitController {
public:
  LLCStreamCommitController(LLCStreamEngine *_se);

  void registerStream(LLCDynStreamPtr dynS);
  void deregisterStream(LLCDynStreamPtr dynS);

  bool hasStreamToCommit() const { return !this->streams.empty(); }

  void commit();

private:
  LLCStreamEngine *se;

  std::list<LLCDynStreamPtr> streams;

  int curRemoteBank() const { return this->se->curRemoteBank(); }
  const char *curRemoteMachineType() const {
    return this->se->curRemoteMachineType();
  }

  /**
   * Try to commit an element for the stream.
   * @return whether we have committed at least one element.
   */
  bool commitStream(LLCDynStreamPtr dynS, bool &migrated);
};

#endif