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
  if (dynS->isTerminated()) {
    LLC_S_PANIC(dynS->getDynamicStreamId(),
                "[Commit] Try to register a terminated stream.");
  }
  dynS->commitController = this;
  this->streams.push_back(dynS);
  LLC_S_DPRINTF(dynS->getDynamicStreamId(), "[Commit] Registered.\n");
}

void LLCStreamCommitController::deregisterStream(LLCDynamicStreamPtr dynS) {
  if (dynS->commitController != this) {
    LLC_S_PANIC(dynS->getDynamicStreamId(),
                "Deregister when not registered at this LLCCommitController.");
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
    if (dynS->commitController != this) {
      LLC_S_PANIC(dynS->getDynamicStreamId(),
                  "Try commit a LLCDynStream not registered here.");
    }
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
  }
  for (auto dynIS : dynS->getIndStreams()) {
    /**
     * If the dynIS issues AfterCommit, we check that the element is
     * ready to be issued.
     * Otherwise, we check that the element is released.
     */
    if (dynIS->shouldIssueAfterCommit()) {
      auto nextCommitIndirectElementIdx = nextCommitElementIdx;
      if (dynIS->isOneIterationBehind()) {
        nextCommitIndirectElementIdx++;
      }
      auto nextCommitElement = dynIS->getElement(nextCommitIndirectElementIdx);
      if (!nextCommitElement) {
        if (dynIS->isElementReleased(nextCommitIndirectElementIdx)) {
          LLC_S_PANIC(
              dynIS->getDynamicStreamId(),
              "[Commit] IndElement %llu already released before commit.",
              nextCommitIndirectElementIdx);
        }
        /**
         * Somehow this element is not yet allocated.
         * In normal cases this should not really happen, as the core should
         * wait for range messages (which happens after elements allocated).
         * However, there is one special case: the total trip count is 1.
         * This is because in the core we have not implement the range sync for
         * the first iteration, and thus the core may commit the first element
         * even before the LLCStreamElement is allocated. In such case, we
         * simply wait.
         */
        if (dynS->hasTotalTripCount() && dynS->getTotalTripCount() == 1 &&
            nextCommitElementIdx == 0) {
          return false;
        }
        LLC_S_PANIC(dynIS->getDynamicStreamId(),
                    "[Commit] Failed to find IndElement %llu to commit.",
                    nextCommitIndirectElementIdx);
      } else {
        if (!nextCommitElement->areBaseElementsReady()) {
          // We can not issue this yet.
          return false;
        }
      }
    } else {
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
   * We can commit this element. So far we just directly commit, and
   * send out the final request for certain streams, e.g. AtomicStream.
   *
   * We also check if we have committed all elements in this message.
   * If so, we send back a done message.
   */
  LLC_S_DPRINTF(dynS->getDynamicStreamId(), "[Commit] Commit element %llu.\n",
                nextCommitElementIdx);
  for (auto dynIS : dynS->getIndStreams()) {
    if (dynIS->shouldIssueAfterCommit()) {
      auto nextCommitIndirectElementIdx = nextCommitElementIdx;
      if (dynIS->isOneIterationBehind()) {
        nextCommitIndirectElementIdx++;
      }
      auto nextCommitElement = dynIS->getElement(nextCommitIndirectElementIdx);
      // We directly issue this.
      LLC_S_DPRINTF(dynS->getDynamicStreamId(),
                    "[Commit] Issue AfterCommit for DynIS %s %llu.\n",
                    dynIS->getDynamicStreamId(), nextCommitIndirectElementIdx);
      this->se->generateIndirectStreamRequest(dynIS, nextCommitElement);
    }
  }
  dynS->commitOneElement();
  if (nextCommitElementIdx >= firstCommitMessage.getEndIdx()) {
    /**
     * We are done with the current commit message.
     * DirectLoadStream without IndirectDependent does not require commit.
     * So far we just issue the StreamDone ideally.
     * TODO: Disable Range-Sync for such DirectLoadStream without IndDep.
     */
    bool ideaStreamDone = false;
    if (dynS->getStaticStream()->isDirectLoadStream() &&
        dynS->indirectStreams.empty()) {
      ideaStreamDone = true;
    }
    LLC_S_DPRINTF(dynS->getDynamicStreamId(),
                  "[Commit] Send back StreamDone for [%llu, %llu(+%d)).\n",
                  firstCommitMessage.getStartIdx(),
                  firstCommitMessage.getEndIdx(),
                  firstCommitMessage.getNumElements());
    this->se->issueStreamDoneToMLC(firstCommitMessage, ideaStreamDone);
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
