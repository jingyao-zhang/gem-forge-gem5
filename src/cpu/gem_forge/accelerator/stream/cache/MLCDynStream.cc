#include "MLCDynStream.hh"

#include "LLCDynStream.hh"
#include "MLCStreamEngine.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCStreamLoopBound.hh"
#include "debug/StreamRangeSync.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

namespace gem5 {

MLCDynStream::MLCDynStream(CacheStreamConfigureDataPtr _configData,
                           ruby::AbstractStreamAwareController *_controller,
                           ruby::MessageBuffer *_responseMsgBuffer,
                           ruby::MessageBuffer *_requestToLLCMsgBuffer,
                           bool _isMLCDirect)
    : stream(_configData->stream),
      strandId(_configData->dynamicId, _configData->strandIdx,
               _configData->totalStrands),
      config(_configData), isPointerChase(_configData->isPointerChase),
      isPUMPrefetch(_configData->isPUMPrefetch),
      isPseudoOffload(_configData->isPseudoOffload), isMLCDirect(_isMLCDirect),
      controller(_controller), responseMsgBuffer(_responseMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer),
      maxNumSlices(_configData->mlcBufferNumSlices), headSliceIdx(0),
      tailSliceIdx(0),
      advanceStreamEvent([this]() -> void { this->advanceStream(); },
                         "MLC::advanceStream",
                         false /*delete after process. */) {

  /**
   * ! You should never call any virtual function in the
   * ! constructor/deconstructor.
   */

  /**
   * Remember our wait type.
   */
  this->isWaiting = this->checkWaiting();

  /**
   * Remember if we require range-sync. The config will also be passed to
   * LLCDynStream.
   */
  auto dynS = this->getCoreDynS();
  this->config->rangeSync = (dynS && dynS->shouldRangeSync());

  MLC_S_DPRINTF_(StreamRangeSync, this->getDynStrandId(),
                 "Wait %s. %s RangeSync.\n", this->to_string(this->isWaiting),
                 this->shouldRangeSync() ? "Enabled" : "Disabled");

  // Schedule the first advanceStreamEvent.
  this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                            Cycles(1));

  // Remember the SendTo configs.
  for (auto &depEdge : this->config->depEdges) {
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
      this->sendToEdges.push_back(depEdge);
    }
  }

  this->nextLoopBoundDoneElemIdx = this->getFirstFloatElemIdx();

  MLC_S_DPRINTF_(StreamRangeSync, this->getDynStrandId(),
                 "MLCDynStream Constructor Done.\n");
}

MLCDynStream::~MLCDynStream() {
  // We got to deschedule the advanceStreamEvent.
  if (this->advanceStreamEvent.scheduled()) {
    this->stream->getCPUDelegator()->deschedule(&this->advanceStreamEvent);
  }
}

MLCDynStream::WaitType MLCDynStream::checkWaiting() const {
  if (this->isPUMPrefetch) {
    return WaitType::Nothing;
  }
  if (this->isPseudoOffload) {
    MLC_S_DPRINTF(this->getDynStrandId(), "PseudoFloat. Wait Nothing.\n");
    return WaitType::Nothing;
  }
  if (this->stream->isStoreStream()) {
    MLC_S_DPRINTF(this->getDynStrandId(), "StoreStream. Wait Ack.\n");
    return WaitType::Ack;
  }
  if (auto dynS = this->getCoreDynS()) {
    if (dynS->shouldCoreSEIssue()) {
      MLC_S_DPRINTF(this->getDynStrandId(), "CoreSE Issue. Wait Data.\n");
      return WaitType::Data;
    } else {
      if (this->stream->isAtomicComputeStream() ||
          this->stream->isUpdateStream()) {
        // These streams writes to memory. Need Ack.
        MLC_S_DPRINTF(this->getDynStrandId(),
                      "CoreSE Not Issue. Atomic/UpdateS. Wait Ack.\n");
        return WaitType::Ack;
      } else if (this->stream->isIndirectReduction()) {
        // If distributed, wait nothing. Otherwise, wait ack.
        if (this->controller->myParams->enable_distributed_indirect_reduce) {
          MLC_S_DPRINTF(this->getDynStrandId(),
                        "CoreSE Not Issue. Dist IndReduce. Wait Nothing.\n");
          return WaitType::Nothing;
        } else {
          MLC_S_DPRINTF(this->getDynStrandId(),
                        "CoreSE Not Issue. Non-Dist IndReduce. Wait Ack.\n");
          return WaitType::Ack;
        }
      } else if (this->stream->isOnlyDirectLoadStream() &&
                 !this->shouldRangeSync()) {
        MLC_S_DPRINTF(this->getDynStrandId(),
                      "CoreSE Not Issue. Only DirectLoadS. Wait Ack.\n");
        return WaitType::Ack;
      } else {
        // Other streams does not write. Need nothing.
        MLC_S_DPRINTF(this->getDynStrandId(),
                      "CoreSE Not Issue. Stream not Write. Wait Nothing.\n");
        return WaitType::Nothing;
      }
    }
  } else {
    /**
     * The dynamic stream is already released. We make an conservative
     * assumption to wait for Data, so that any delayed requests can get the
     * correct slice, and later received a dummy response when we received the
     * StreamEnd.
     */
    MLC_S_DPRINTF(this->getDynStrandId(), "No CoreDynS. Assume Wait Data.\n");
    return WaitType::Data;
  }
}

void MLCDynStream::endStream() {
  MLC_S_DPRINTF(this->getDynStrandId(), "Ended with # slices %d.\n",
                this->slices.size());
  for (auto &slice : this->slices) {
    MLC_SLICE_DPRINTF(
        slice.sliceId, "Ended with CoreStatus %s.\n",
        MLCStreamSlice::convertCoreStatusToString(slice.coreStatus));
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA) {
      // Make a dummy response.
      // Ignore whether the data is ready.
      // ! For indirect stream, the sliceId may not have vaddr.
      // ! In such case, we set it from core's sliceId.
      // TODO: Fix this in a more rigorous way.
      if (slice.sliceId.vaddr == 0) {
        slice.sliceId.vaddr = slice.coreSliceId.vaddr;
      }
      this->makeResponse(slice);
    }
  }
}

void MLCDynStream::recvCoreReq(const DynStreamSliceId &sliceId) {
  MLC_SLICE_DPRINTF(sliceId, "Receive request to %#x. Tail %lu.\n",
                    sliceId.vaddr, this->tailSliceIdx);

  auto slice = this->findSliceForCoreRequest(sliceId);
  assert(slice->coreStatus == MLCStreamSlice::CoreStatusE::NONE &&
         "Already seen a request.");
  MLC_SLICE_DPRINTF(slice->sliceId, "Matched to request.\n");
  slice->coreStatus = MLCStreamSlice::CoreStatusE::WAIT_DATA;
  slice->coreWaitCycle = this->controller->curCycle();
  slice->coreSliceId = sliceId;
  if (slice->dataReady) {
    // Sanity check the address.
    // ! Core is line address.
    if (slice->coreSliceId.vaddr !=
        ruby::makeLineAddress(slice->sliceId.vaddr)) {
      MLC_SLICE_PANIC(sliceId, "Mismatch between Core %#x and LLC %#x.\n",
                      slice->coreSliceId.vaddr, slice->sliceId.vaddr);
    }
    this->makeResponse(*slice);
  }
  this->advanceStream();
}

void MLCDynStream::recvCoreReqHit(const DynStreamSliceId &sliceId) {
  MLC_SLICE_DPRINTF(sliceId, "Recv req hit to %#x.\n", sliceId.vaddr);

  auto slice = this->findSliceForCoreRequest(sliceId);
  if (slice->coreStatus != MLCStreamSlice::CoreStatusE::NONE) {
    MLC_SLICE_PANIC(sliceId, "Already seen a request.");
  }
  slice->coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  slice->coreSliceId = sliceId;
  this->advanceStream();
}

bool MLCDynStream::checkRecvDynSForPop(const DynStreamSliceId &sliceId) {

  assert(sliceId.getEndIdx() > 0 && "Impossible.");
  auto strandElemIdx = sliceId.getEndIdx() - 1;

  // Handle merged broadcast including myself.
  auto broadcastStrands = this->config->broadcastStrands;
  broadcastStrands.insert(broadcastStrands.begin(), this->config);

  // Check all the receiver.
  for (auto &config : broadcastStrands) {

    for (const auto &dep : config->depEdges) {

      if (dep.type != CacheStreamConfigureData::DepEdge::Type::SendTo) {
        continue;
      }

      auto translation =
          config->translateSendToRecv(dep, config, strandElemIdx);

      auto recvStrandId = std::get<0>(translation);
      auto recvStrandElemIdx = std::get<1>(translation);

      auto remoteRecvS = LLCDynStream::getLLCStream(recvStrandId);
      if (!remoteRecvS) {
        MLC_S_PANIC(this->getDynStrandId(), "LLCRecvDynS already released: %s.",
                    recvStrandId);
      }

      auto recvInitStrandElemIdx = remoteRecvS->getNextInitElementIdx();

      if (recvInitStrandElemIdx >= recvStrandElemIdx) {
        continue;
      }

      /**
       * The RecvDynS has not allocated this yet.
       */
      auto se = this->controller->getMLCStreamEngine();
      auto dynId = this->getDynStrandId();
      auto elemInitCallback = [se, dynId](const DynStrandId &dynStreamId,
                                          uint64_t elementIdx) -> void {
        if (auto dynS = se->getStreamFromStrandId(dynId)) {
          dynS->popBlocked = false;
          dynS->scheduleAdvanceStream();
        } else {
          // This MLC stream already released.
        }
      };
      this->popBlocked = true;
      MLC_SLICE_DPRINTF(
          sliceId,
          "[DelayPop] RecvElemIdx MLC BrdStrand %d %lu -> LLC %s%lu > "
          "%lu. RegisterCB at %lu\n",
          config->strandIdx, strandElemIdx, recvStrandId, recvStrandElemIdx,
          recvInitStrandElemIdx, recvStrandElemIdx);
      remoteRecvS->registerElemInitCallback(recvStrandElemIdx,
                                            elemInitCallback);
      return false;
    }
  }

  return true;
}

bool MLCDynStream::tryPopStream() {
  /**
   * So far we don't have a synchronization scheme between MLC and LLC if there
   * is no CoreUser, and that causes performance drop due to running too ahead.
   * Therefore, we try to have an ideal check that the LLCStream is ahead of me.
   * We only do this for MLCDirectStream.
   */
  if (this->popBlocked) {
    return false;
  }

  uint64_t remoteProgressSliceIdx = UINT64_MAX;
  LLCDynStreamPtr remoteDynSLeastProgress = nullptr;

  uint64_t remoteDynISProgressElemIdx = UINT64_MAX;
  // LLCDynStreamPtr llcDynISLeastProgress = nullptr;

  if (this->isMLCDirect && !this->shouldRangeSync() &&
      this->controller->isStreamIdeaMLCPopCheckEnabled()) {

    auto remoteDynS = LLCDynStream::getLLCStream(this->getDynStrandId());
    if (!remoteDynS) {
      MLC_S_PANIC(this->getDynStrandId(), "RemoteDynS already released.");
    }

    remoteProgressSliceIdx = remoteDynS->getNextAllocSliceIdx();
    remoteDynSLeastProgress = remoteDynS;

    /**
     * We are also going to limit llcProgressElementIdx to the unreleased
     * IndirectElementIdx + 1024 / MemElementSize.
     */
    const int remoteISAliveBytes = 0;
    for (auto remoteDynIS : remoteDynS->getAllIndStreams()) {

      auto unreleasedElemIdx = remoteDynIS->getNextUnreleasedElemIdx();

      auto dynISElemOffset =
          remoteISAliveBytes / remoteDynIS->getMemElementSize();

      if (unreleasedElemIdx + dynISElemOffset < remoteDynISProgressElemIdx) {

        MLC_S_DPRINTF(
            this->getDynStrandId(),
            "Smaller RemoteDynIS %s UnreleaseElem %llu + %llu < %llu.\n",
            remoteDynIS->getDynStrandId(), unreleasedElemIdx, dynISElemOffset,
            remoteDynISProgressElemIdx);

        remoteDynISProgressElemIdx = unreleasedElemIdx + dynISElemOffset;
        // llcDynISLeastProgress = llcDynIS;
      }
    }
  }

  /**
   * Maybe let's make release in order.
   * The slice is released once the core status is DONE or FAULTED.
   */
  bool popped = false;
  while (!this->slices.empty()) {
    const auto &slice = this->slices.front();
    if (slice.coreStatus != MLCStreamSlice::CoreStatusE::DONE &&
        slice.coreStatus != MLCStreamSlice::CoreStatusE::FAULTED) {
      // This slice is not done.
      break;
    }

    auto mlcHeadSliceIdx = this->tailSliceIdx - this->slices.size();
    auto mlcHeadSliceEndElemIdx = slice.sliceId.getEndIdx();

    /**
     * Check all the requirements.
     */
    if (mlcHeadSliceIdx > remoteProgressSliceIdx) {
      MLC_SLICE_DPRINTF(slice.sliceId,
                        "[DelayPop] SelfSliceIdx MLC %llu > LLC %llu.\n",
                        mlcHeadSliceIdx, remoteProgressSliceIdx);
      auto se = this->controller->getMLCStreamEngine();
      auto dynId = this->getDynStrandId();
      auto sliceAllocCallback = [se, dynId](const DynStreamId &dynStreamId,
                                            uint64_t sliceIdx) -> void {
        if (auto dynS = se->getStreamFromStrandId(dynId)) {
          dynS->popBlocked = false;
          dynS->scheduleAdvanceStream();
        } else {
          // This MLC stream already released.
        }
      };
      this->popBlocked = true;
      remoteDynSLeastProgress->registerSliceAllocCallback(mlcHeadSliceIdx,
                                                          sliceAllocCallback);
      break;
    }

    if (mlcHeadSliceEndElemIdx > remoteDynISProgressElemIdx) {
      MLC_SLICE_DPRINTF(slice.sliceId,
                        "[DelayPop] ISElemIdx MLC %llu > LLC %llu.\n",
                        mlcHeadSliceEndElemIdx, remoteDynISProgressElemIdx);
      this->scheduleAdvanceStream();
      break;
    }

    if (!this->checkRecvDynSForPop(slice.sliceId)) {
      assert(this->popBlocked && "Should be blocked by RecvS.");
      break;
    }

    this->popOneSlice();
    popped = true;
  }
  return popped;
}

void MLCDynStream::popOneSlice() {
  const auto &slice = this->slices.front();
  MLC_SLICE_DPRINTF(slice.sliceId, "Pop.\n");

  // Update the statistics.
  if (slice.coreWaitCycle != 0 && slice.dataReadyCycle != 0) {
    auto &streamStats = this->stream->statistic;
    if (slice.coreWaitCycle > slice.dataReadyCycle) {
      // Early.
      streamStats.numMLCEarlySlice++;
      streamStats.numMLCEarlyCycle +=
          slice.coreWaitCycle - slice.dataReadyCycle;
    } else {
      // Late.
      streamStats.numMLCLateSlice++;
      streamStats.numMLCLateCycle += slice.dataReadyCycle - slice.coreWaitCycle;
    }
  }

  this->headSliceIdx++;
  this->slices.pop_front();
}

void MLCDynStream::makeResponse(MLCStreamSlice &slice) {
  assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA &&
         "Element core status should be WAIT_DATA to make response.");
  Addr paddr = this->translateVAddr(slice.sliceId.vaddr);
  auto paddrLine = ruby::makeLineAddress(paddr);

  auto selfMachineId = this->controller->getMachineID();
  auto upperMachineId =
      ruby::MachineID(static_cast<ruby::MachineType>(selfMachineId.type - 1),
                      selfMachineId.num);
  auto msg =
      std::make_shared<ruby::CoherenceMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Class = ruby::CoherenceClass_DATA_EXCLUSIVE;
  msg->m_Sender = selfMachineId;
  msg->m_Dest = upperMachineId;
  msg->m_MessageSize = ruby::MessageSizeType_Response_Data;
  msg->m_DataBlk = slice.dataBlock;

  /**
   * Floating AtomicComputeStream and LoadComputeStream and UpdateStream must
   * use STREAM_FROM_MLC type as they bypass private cache and must be served by
   * MLC SE.
   */
  if (this->stream->isAtomicComputeStream() ||
      this->stream->isLoadComputeStream() || this->stream->isUpdateStream()) {
    msg->m_Class = ruby::CoherenceClass_STREAM_FROM_MLC;
  }

  // Show the data.
  if (Debug::DEBUG_TYPE) {
    std::stringstream ss;
    auto lineOffset =
        slice.sliceId.vaddr % ruby::RubySystem::getBlockSizeBytes();
    auto dataStr = GemForgeUtils::dataToString(
        slice.dataBlock.getData(lineOffset, slice.sliceId.getSize()),
        slice.sliceId.getSize());
    MLC_SLICE_DPRINTF(slice.sliceId,
                      "Make response vaddr %#x size %d data %s.\n",
                      slice.sliceId.vaddr, slice.sliceId.getSize(), dataStr);
  }
  // The latency should be consistency with the cache controller.
  // However, I still failed to find a clean way to exponse this info
  // to the stream engine. So far I manually set it to the default
  // value from the L1 cache controller.
  // TODO: Make it consistent with the cache controller.
  Cycles latency(2);
  this->responseMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                   this->controller->cyclesToTicks(latency));

  /**
   * Special case for AtomicStream with RangeSync:
   * we should expect an Ack once committed.
   * So here we transit to WAIT_ACK state.
   */
  if (this->getStaticStream()->isAtomicComputeStream() &&
      this->shouldRangeSync()) {
    slice.coreStatus = MLCStreamSlice::CoreStatusE::WAIT_ACK;
  } else {
    // Set the core status to DONE.
    slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  }
  // Update the stats in core SE.
  this->stream->se->numMLCResponse++;
}

void MLCDynStream::makeAck(MLCStreamSlice &slice) {
  assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_ACK &&
         "Elem core status should be WAIT_ACK to make ack.");
  slice.coreStatus = MLCStreamSlice::CoreStatusE::ACK_READY;
  MLC_SLICE_DPRINTF(slice.sliceId, "AckReady. Header %s HeaderCoreStatus %s.\n",
                    this->slices.front().sliceId,
                    MLCStreamSlice::convertCoreStatusToString(
                        this->slices.front().coreStatus));
  // Send back ack.
  auto dynS = this->getCoreDynS();
  // for (auto &ackSlice : this->slices) {
  //   if (ackSlice.coreStatus == MLCStreamSlice::CoreStatusE::DONE) {
  //     continue;
  //   }
  //   if (ackSlice.coreStatus != MLCStreamSlice::CoreStatusE::ACK_READY) {
  //     continue;
  //   }
  {
    auto &ackSlice = slice;
    const auto &ackSliceId = ackSlice.sliceId;
    // Set the core status to DONE.
    ackSlice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
    for (auto strandElemIdx = ackSliceId.getStartIdx();
         strandElemIdx < ackSliceId.getEndIdx(); ++strandElemIdx) {
      if (std::dynamic_pointer_cast<LinearAddrGenCallback>(
              this->config->addrGenCallback)) {
        auto elemVAddr =
            this->config->addrGenCallback
                ->genAddr(strandElemIdx, this->config->addrGenFormalParams,
                          getStreamValueFail)
                .uint64();
        if (elemVAddr + this->config->elementSize >
            ackSliceId.vaddr + ackSliceId.getSize()) {
          // This element spans to next slice, do not ack here.
          MLC_SLICE_DPRINTF(ackSliceId,
                            "Skipping Ack for multi-slice elem %llu [%#x, +%d) "
                            "slice [%#x, +%d).\n",
                            strandElemIdx, elemVAddr, this->config->elementSize,
                            ackSliceId.vaddr, ackSliceId.getSize());
          continue;
        }
      }
      if (!dynS) {
        /**
         * Make ack when the CoreDynS is already released:
         * 1. The stream is rewinded.
         * 2. The second Ack for RangeSync AtomicStream.
         * 3. LoopBound cut the stream early.
         */
        if (this->isCoreDynStreamRewinded()) {
          continue;
        }
        if (this->shouldRangeSync() && this->stream->isAtomicComputeStream()) {
          continue;
        }
        if (this->loopBoundBrokenOut) {
          continue;
        }
        MLC_SLICE_PANIC(ackSliceId, "MakeAck when dynS has been released.");
      }
      auto streamElemIdx =
          this->config->getStreamElemIdxFromStrandElemIdx(strandElemIdx);
      MLC_SLICE_DPRINTF(slice.sliceId,
                        "Ack for StrandElem %llu StreamElem %llu.\n",
                        strandElemIdx, streamElemIdx);
      dynS->ackCacheElement(streamElemIdx);

      /**
       * We call ElementAckCallback here.
       */
      auto elemAckCallbackIter = this->elemAckCallbacks.find(strandElemIdx);
      if (elemAckCallbackIter != this->elemAckCallbacks.end()) {
        for (auto &callback : elemAckCallbackIter->second) {
          callback(this->getDynStreamId(), strandElemIdx);
        }
        this->elemAckCallbacks.erase(elemAckCallbackIter);
      }
    }
  }
}

Addr MLCDynStream::translateVAddr(Addr vaddr) const {
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }
  return paddr;
}

void MLCDynStream::readBlob(Addr vaddr, uint8_t *data, int size) const {
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  cpuDelegator->readFromMem(vaddr, size, data);
}

void MLCDynStream::receiveStreamRange(const DynStreamAddressRangePtr &range) {
  // We simply notify the dynamic streams in core for now.
  if (!this->shouldRangeSync()) {
    MLC_S_PANIC(this->getDynStrandId(),
                "Receive StreamRange when RangeSync not required.");
  }
  if (auto dynS = this->getCoreDynS()) {
    dynS->receiveStreamRange(range);
  }
}

void MLCDynStream::receiveStreamDone(const DynStreamSliceId &sliceId) {
  MLC_S_PANIC(this->getDynStrandId(), "receiveStreamDone not implemented.");
}

void MLCDynStream::scheduleAdvanceStream() {
  if (!this->advanceStreamEvent.scheduled()) {
    this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                              Cycles(1));
  }
}

std::string
MLCDynStream::MLCStreamSlice::convertCoreStatusToString(CoreStatusE status) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (status) {
    Case(NONE);
    Case(WAIT_DATA);
    Case(WAIT_ACK);
    Case(ACK_READY);
    Case(DONE);
    Case(FAULTED);
#undef Case
  default:
    panic("Invalid MLCStreamSlice::CoreStatus.");
  }
}

bool MLCDynStream::isElementAcked(uint64_t strandElemIdx) const {

  // We should really check the Slice status. However, here I just check the
  // CoreDynS.
  auto streamElemIdx =
      this->config->getStreamElemIdxFromStrandElemIdx(strandElemIdx);
  auto coreDynS = this->getStaticStream()->getDynStream(this->getDynStreamId());
  assert(coreDynS && "CoreDynS already released when checking ElemAcked.");
  return coreDynS->cacheAckedElements.count(streamElemIdx);
}

void MLCDynStream::registerElementAckCallback(uint64_t strandElemIdx,
                                              ElementCallback callback) {
  if (this->isElementAcked(strandElemIdx)) {
    MLC_S_PANIC(this->getDynStrandId(),
                "Register ElementAckCallback for Acked Element %llu.",
                strandElemIdx);
  }
  this->elemAckCallbacks
      .emplace(std::piecewise_construct, std::forward_as_tuple(strandElemIdx),
               std::forward_as_tuple())
      .first->second.push_back(callback);
}

void MLCDynStream::receiveStreamLoopBoundResult(uint64_t elemIdx,
                                                bool brokenOut) {

  assert(this->hasLoopBound() && "RecvLoopBoundResult without LoopBound.");

  MLC_S_DPRINTF_(MLCStreamLoopBound, this->getDynStrandId(),
                 "[MLCLoopBound] Recv %lu Broken %d.\n", elemIdx, brokenOut);

  [[maybe_unused]] auto added =
      this->loopBoundResults.emplace(elemIdx, brokenOut).second;
  assert(added && "Duplicated LoopBoundResult.");

  this->advanceStreamLoopBound();
}

void MLCDynStream::advanceStreamLoopBound() {

  assert(this->hasLoopBound() && "advanceStreamLoopBound without LoopBound.");

  while (!this->hasTotalTripCount() && !this->loopBoundResults.empty() &&
         this->loopBoundResults.begin()->first ==
             this->nextLoopBoundDoneElemIdx) {

    auto elemIdx = this->nextLoopBoundDoneElemIdx;
    auto broken = this->loopBoundResults.begin()->second;

    MLC_S_DPRINTF_(MLCStreamLoopBound, this->getDynStrandId(),
                   "[MLCLoopBound] Advance %lu Broken %d.\n", elemIdx, broken);

    this->nextLoopBoundDoneElemIdx++;
    this->loopBoundResults.erase(this->loopBoundResults.begin());

    auto tripCount = elemIdx + 1;

    // Tell the core.
    this->getStaticStream()->getSE()->receiveOffloadedLoopBoundRet(
        this->getDynStreamId(), tripCount, broken);

    if (!broken) {
      continue;
    }

    /**
     * The following implementation is idealized:
     * 1. Get localtion of RemoteS and halt it.
     * 2. Set the trip count for myself.
     */

    auto llcDynS = LLCDynStream::getLLCStream(this->getDynStrandId());
    assert(llcDynS && "No LLCDynS when broken out.");
    auto llcRootDynS = llcDynS->rootStream ? llcDynS->rootStream : llcDynS;

    llcRootDynS->breakOutLoop(tripCount);
    this->breakOutLoop(tripCount);
  }
}

void MLCDynStream::panicDump() const {
  MLC_S_HACK(this->strandId,
             "-------------------Panic Dump--------------------\n");
  for (const auto &slice : this->slices) {
    MLC_SLICE_HACK(slice.sliceId, "VAddr %#x Data %d Core %s.\n",
                   slice.sliceId.vaddr, slice.dataReady,
                   MLCStreamSlice::convertCoreStatusToString(slice.coreStatus));
  }
}

void MLCDynStream::sample() const {

  auto S = this->getStaticStream();
  auto &statistic = S->statistic;
  auto &staticStats = statistic.getStaticStat();

  auto aliveSlices = this->slices.size();

  auto nextCreditElemIdx = this->getNextCreditElemIdx();
  int creditSentSlices = 0;

  std::array<int, MLCStreamSlice::CoreStatusE::NUM_CORE_STATUS>
      sliceCoreStatus = {0};

  for (const auto &slice : this->slices) {
    if (slice.sliceId.getStartIdx() < nextCreditElemIdx) {
      // This slice has the credit sent.
      creditSentSlices++;
    }
    sliceCoreStatus[slice.coreStatus]++;
  }
  int creditNotSentSlices = aliveSlices - creditSentSlices;

#define sample(sampler, v)                                                     \
  statistic.sampler.sample(v);                                                 \
  staticStats.sampler.sample(this->curCycle(), v);

  sample(localAliveSlice, aliveSlices);
  sample(localCreditNotSentSlice, creditNotSentSlices);
  sample(localCreditSentSlice, creditSentSlices);
  sample(localWaitDataSlice,
         sliceCoreStatus[MLCStreamSlice::CoreStatusE::WAIT_DATA]);
  sample(localWaitAckSlice,
         sliceCoreStatus[MLCStreamSlice::CoreStatusE::WAIT_ACK]);
  sample(localAckReadySlice,
         sliceCoreStatus[MLCStreamSlice::CoreStatusE::ACK_READY]);
  sample(localDoneSlice, sliceCoreStatus[MLCStreamSlice::CoreStatusE::DONE]);

#undef sample
}

} // namespace gem5
