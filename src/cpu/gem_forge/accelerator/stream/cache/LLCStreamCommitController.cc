#include "LLCStreamCommitController.hh"

#include "debug/StreamRangeSync.hh"
#define DEBUG_TYPE StreamRangeSync
#include "../stream_log.hh"

LLCStreamCommitController::LLCStreamCommitController(LLCStreamEngine *_se)
    : se(_se) {}

void LLCStreamCommitController::registerStream(LLCDynamicStreamPtr dynS) {
  if (dynS->commitController) {
    LLC_S_PANIC(dynS->getDynamicStreamId(),
                "Already has registered at CommitController.");
  }
  dynS->commitController = this;
  this->streams.push_back(dynS);
  LLC_S_DPRINTF(dynS->getDynamicStreamId(), "[Commit] Registered.\n");
}

void LLCStreamCommitController::deregisterStream(LLCDynamicStreamPtr dynS) {
  if (dynS->commitController != this) {
    LLC_S_PANIC(dynS->getDynamicStreamId(),
                "Not registered at this LLCStreamCommitController.");
  }
  dynS->commitController = nullptr;
  for (auto iter = this->streams.begin(), end = this->streams.end();
       iter != end; ++iter) {
    auto S = *iter;
    if (S == dynS) {
      LLC_S_DPRINTF(dynS->getDynamicStreamId(), "[Commit] Deregistered.\n");
      this->streams.erase(iter);
      return;
    }
  }
  LLC_S_PANIC(dynS->getDynamicStreamId(), "Failed to find registered stream.");
}

void LLCStreamCommitController::commit() {
  int numCommitted = 0;
  const int commitWidth = 1;
  std::vector<LLCDynamicStreamPtr> migratedStreams;
  for (auto dynS : this->streams) {
    if (numCommitted >= commitWidth) {
      break;
    }
    bool migrated = false;
    if (this->commitStream(dynS, migrated)) {
      numCommitted++;
      if (migrated) {
        migratedStreams.push_back(dynS);
      }
    }
  }
  for (auto dynS : migratedStreams) {
    this->deregisterStream(dynS);
  }
}

bool LLCStreamCommitController::commitStream(LLCDynamicStreamPtr dynS,
                                             bool &migrated) {
  auto &commitMessages = dynS->commitMessages;
  auto &nextCommitElementIdx = dynS->nextCommitElementIdx;
  if (commitMessages.empty()) {
    // if (nextCommitElementIdx > 6267) {
    //   LLC_S_DPRINTF(dynS->getDynamicStreamId(),
    //                 "S not no commit message %llu, numElements %d.\n",
    //                 nextCommitElementIdx, dynS->idxToElementMap.size());
    // }
    return false;
  }
  auto &firstCommitMessage = commitMessages.front();
  if (nextCommitElementIdx < firstCommitMessage.getStartIdx()) {
    // Some how we are still waiting for the commit messages.
    // if (nextCommitElementIdx > 6267) {
    //   LLC_S_DPRINTF(dynS->getDynamicStreamId(),
    //                 "S not future commit message %llu, numElements %d.\n",
    //                 nextCommitElementIdx, dynS->idxToElementMap.size());
    // }
    return false;
  }
  if (!firstCommitMessage.elementRange.contains(nextCommitElementIdx)) {
    LLC_S_PANIC(dynS->getDynamicStreamId(),
                "[Commit] Stale CommitMessages [%llu, %llu), Next %llu.",
                firstCommitMessage.getStartIdx(),
                firstCommitMessage.getEndIdx(), nextCommitElementIdx);
  }

  /**
   * We need to check that the DynamicStream has released its element.
   * Due to delay releasing, we hack here to direct commit last element.
   */
  if (dynS->hasTotalTripCount() &&
      nextCommitElementIdx + 1 < dynS->getTotalTripCount()) {
    if (!dynS->isElementReleased(nextCommitElementIdx)) {
      // if (nextCommitElementIdx > 6267) {
      //   LLC_S_DPRINTF(dynS->getDynamicStreamId(),
      //                 "S not released %llu, numElements %d.\n",
      //                 nextCommitElementIdx, dynS->idxToElementMap.size());
      // }
      return false;
    }
    for (auto dynIS : dynS->indirectStreams) {
      if (!dynIS->isElementReleased(nextCommitElementIdx)) {
        if (nextCommitElementIdx > 6267) {
          LLC_S_DPRINTF(dynS->getDynamicStreamId(), "IS not released %llu.\n",
                        nextCommitElementIdx);
        }
        return false;
      }
    }
  }

  /**
   * We can commit this element. So far we just directly commit.
   * We also check if we have committed all elements in this message.
   * If so, we send back a done message.
   */
  LLC_S_DPRINTF(dynS->getDynamicStreamId(), "[Commit] Commit element %llu.\n",
                nextCommitElementIdx);
  nextCommitElementIdx++;
  if (nextCommitElementIdx >= firstCommitMessage.getEndIdx()) {
    // We are done with the current commit message.
    LLC_S_DPRINTF(dynS->getDynamicStreamId(),
                  "[Commit] Send back StreamDone for [%llu, %llu(+%d)).\n",
                  firstCommitMessage.getStartIdx(),
                  firstCommitMessage.getEndIdx(),
                  firstCommitMessage.getNumElements());
    this->se->issueStreamDoneToMLC(firstCommitMessage);
    commitMessages.pop_front();
  }
  /**
   * We need to check that if we have migrate the stream to next bank to commit.
   */
  auto nextElementVAddr = dynS->getElementVAddr(nextCommitElementIdx);
  Addr nextElementPAddr;
  if (!dynS->translateToPAddr(nextElementVAddr, nextElementPAddr)) {
    LLC_S_PANIC(dynS->getDynamicStreamId(),
                "Failed to translate NextCommitElement %llu VAddr %#x.",
                nextCommitElementIdx, nextElementVAddr);
  }
  if (!this->se->isPAddrHandledByMe(nextElementPAddr)) {
    // We have to migrate it.
    this->se->migrateStreamCommit(dynS, nextElementPAddr);
    migrated = true;
  }
  return true;
}
