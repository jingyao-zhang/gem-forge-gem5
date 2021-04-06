#include "AbstractStreamAwareController.hh"

#include "RubySlicc_ComponentMapping.hh"

AbstractStreamAwareController::GlobalMap
    AbstractStreamAwareController::globalMap;
std::list<AbstractStreamAwareController *>
    AbstractStreamAwareController::globalList;

AbstractStreamAwareController::AbstractStreamAwareController(const Params *p)
    : AbstractController(p), myParams(p), pcReqRecorder(p->name),
      llcSelectLowBit(p->llc_select_low_bit),
      llcSelectNumBits(p->llc_select_num_bits),
      numCoresPerRow(p->num_cores_per_row),
      enableStreamFloat(p->enable_stream_float),
      enableStreamSubline(p->enable_stream_subline),
      enableStreamIdeaAck(p->enable_stream_idea_ack),
      enableStreamIdeaStore(p->enable_stream_idea_store),
      enableStreamAdvanceMigrate(p->enable_stream_advance_migrate),
      enableStreamMulticast(p->enable_stream_multicast),
      streamMulticastGroupSize(p->stream_multicast_group_size),
      streamMulticastGroupPerRow(1),
      mlcStreamBufferInitNumEntries(p->mlc_stream_buffer_init_num_entries) {
  if (this->streamMulticastGroupSize > 0) {
    this->streamMulticastGroupPerRow =
        (this->numCoresPerRow + this->streamMulticastGroupSize - 1) /
        this->streamMulticastGroupSize;
  }
  if (p->stream_multicast_issue_policy == "any") {
    this->streamMulticastIssuePolicy = MulticastIssuePolicy::Any;
  } else if (p->stream_multicast_issue_policy == "first_allocated") {
    this->streamMulticastIssuePolicy = MulticastIssuePolicy::FirstAllocated;
  } else if (p->stream_multicast_issue_policy == "first") {
    this->streamMulticastIssuePolicy = MulticastIssuePolicy::First;
  } else {
    panic("Illegal StreamMulticastIssuePolicy %s.\n",
          p->stream_multicast_issue_policy);
  }
  /**
   * Register myself to the global map.
   */
  registerController(this);
}

void AbstractStreamAwareController::regStats() {
  AbstractController::regStats();
  m_statCoreReq.name(name() + ".coreRequests")
      .desc("number of core requests seen")
      .flags(Stats::nozero);
  m_statCoreLoadReq.name(name() + ".coreLoadRequests")
      .desc("number of core load requests seen")
      .flags(Stats::nozero);
  m_statCoreStreamReq.name(name() + ".coreStreamRequests")
      .desc("number of core stream requests seen")
      .flags(Stats::nozero);
  m_statCoreStreamLoadReq.name(name() + ".coreStreamLoadRequests")
      .desc("number of core stream load requests seen")
      .flags(Stats::nozero);
  m_statLLCStreamReq.name(name() + ".llcStreamRequests")
      .desc("number of llc stream requests seen")
      .flags(Stats::nozero);
  m_statLLCIndStreamReq.name(name() + ".llcIndStreamRequests")
      .desc("number of llc indirect stream requests seen")
      .flags(Stats::nozero);
  m_statLLCMulticastStreamReq.name(name() + ".llcMulticastStreamRequests")
      .desc("number of llc multicast stream requests seen")
      .flags(Stats::nozero);
  m_statLLCScheduledComputation.name(name() + ".llcScheduledStreamComputation")
      .desc("number of llc stream computation scheduled")
      .flags(Stats::nozero);
  m_statLLCScheduledComputeMicroOps.name(name() + ".llcScheduledStreamComputeMicroOps")
      .desc("number of llc stream computation microops scheduled")
      .flags(Stats::nozero);
  m_statLLCPerformedAtomics.name(name() + ".llcStreamAtomicsPerformed")
      .desc("number of llc stream atomics performed")
      .flags(Stats::nozero);
  m_statLLCCommittedAtomics.name(name() + ".llcStreamAtomicsCommitted")
      .desc("number of llc stream atomics committed")
      .flags(Stats::nozero);
  m_statLLCLockedAtomics.name(name() + ".llcStreamAtomicsLocked")
      .desc("number of llc stream atomics locked")
      .flags(Stats::nozero);
  m_statLLCUnlockedAtomics.name(name() + ".llcStreamAtomicsUnlocked")
      .desc("number of llc stream atomics unlocked")
      .flags(Stats::nozero);
  m_statLLCLineConflictAtomics.name(name() + ".llcStreamAtomicsLineConflict")
      .desc("number of llc stream atomics that has line conflict")
      .flags(Stats::nozero);
  m_statLLCRealConflictAtomics.name(name() + ".llcStreamAtomicsRealConflict")
      .desc("number of llc stream atomics that has real conflict")
      .flags(Stats::nozero);
  m_statLLCXAWConflictAtomics.name(name() + ".llcStreamAtomicsXAWConflict")
      .desc("number of llc stream atomics that has X-after-write conflict")
      .flags(Stats::nozero);
  m_statLLCRealXAWConflictAtomics.name(name() + ".llcStreamAtomicsRealXAWConflict")
      .desc("number of llc stream atomics that has real X-after-write conflict")
      .flags(Stats::nozero);
  m_statLLCDeadlockAtomics.name(name() + ".llcStreamAtomicsDeadlock")
      .desc("number of llc stream atomics that triggers deadlock")
      .flags(Stats::nozero);

  m_statLLCNumDirectStreams.init(1, 512, 16)
      .name(name() + ".llcNumDirectStreams")
      .desc("Sample of number of LLC direct streams.")
      .flags(Stats::pdf);

  // Register stats callback.
  Stats::registerResetCallback(
      new MakeCallback<PCRequestRecorder, &PCRequestRecorder::reset>(
          &this->pcReqRecorder, true /* auto delete */));
  Stats::registerDumpCallback(
      new MakeCallback<PCRequestRecorder, &PCRequestRecorder::dump>(
          &this->pcReqRecorder, true /* auto delete */));
}

MachineID
AbstractStreamAwareController::mapAddressToLLC(Addr addr,
                                               MachineType mtype) const {
  // Ideally we should check mtype to be LLC or directory, etc.
  // But here I ignore it.
  return mapAddressToRange(addr, mtype, this->llcSelectLowBit,
                           this->llcSelectNumBits, 0 /* cluster_id. */
  );
}

Addr AbstractStreamAwareController::getAddressToOurLLC() const {
  // Make it simple.
  return this->getMachineID().num << this->llcSelectLowBit;
}

GemForgeCPUDelegator *AbstractStreamAwareController::getCPUDelegator() {
  if (!this->cpu) {
    // Start to search for the CPU.
    for (auto so : this->getSimObjectList()) {
      auto cpu = dynamic_cast<BaseCPU *>(so);
      if (!cpu) {
        continue;
      }
      if (cpu->cpuId() != this->getMachineID().num) {
        // This is not my local cpu.
        continue;
      }
      if (!cpu->getCPUDelegator()) {
        // This one has no GemForgeCPUDelegator, should not be our target.
        continue;
      }
      this->cpu = cpu;
      break;
    }
  }
  assert(this->cpu && "Failed to find CPU.");
  auto cpuDelegator = this->cpu->getCPUDelegator();
  assert(cpuDelegator && "Missing CPUDelegator.");
  return cpuDelegator;
}

AbstractStreamAwareController *
AbstractStreamAwareController::getController(MachineID machineId) {

  /**
   * Lazy construction of the globalMap.
   * As during construction, getMachinId() is not available.
   */
  if (globalMap.empty()) {
    for (auto controller : globalList) {
      auto machineId = controller->getMachineID();
      auto &nodeMap = globalMap
                          .emplace(std::piecewise_construct,
                                   std::forward_as_tuple(machineId.getType()),
                                   std::forward_as_tuple())
                          .first->second;
      assert(nodeMap.emplace(machineId.getNum(), controller).second);
    }
  }

  auto typeIter = globalMap.find(machineId.getType());
  if (typeIter != globalMap.end()) {
    auto nodeIter = typeIter->second.find(machineId.getNum());
    if (nodeIter != typeIter->second.end()) {
      return nodeIter->second;
    }
  }
  panic("Failed to find StreamAwareController %s.\n", machineId);
}

void AbstractStreamAwareController::registerController(
    AbstractStreamAwareController *controller) {
  // Add to the list first.
  globalList.emplace_back(controller);
}

void AbstractStreamAwareController::recordPCReq(
    const RequestStatisticPtr &reqStat) const {
  Addr pc = 0;
  bool isStream = false;
  const char *streamName = nullptr;
  if (reqStat) {
    pc = reqStat->pc;
    isStream = reqStat->isStream;
    streamName = reqStat->streamName;
  }
  // For now we have no latency information for AbstractController.
  // And simply use LD request.
  Cycles latency(1);
  this->pcReqRecorder.recordReq(pc, RubyRequestType_LD, isStream, streamName,
                                latency);
}

void AbstractStreamAwareController::recordDeallocateNoReuseReqStats(
    const RequestStatisticPtr &reqStat, CacheMemory &cache) const {
  // Record msg stats.
  auto nocCtrl = reqStat->nocControlMessages;
  auto nocCtrlEvict = reqStat->nocControlEvictMessages;
  auto nocData = reqStat->nocDataMessages;
  cache.m_deallocated_no_reuse_noc_control_message += nocCtrl;
  cache.m_deallocated_no_reuse_noc_control_evict_message += nocCtrlEvict;
  cache.m_deallocated_no_reuse_noc_data_message += nocData;
  if (reqStat->isStream) {
    // Record this is from stream.
    cache.m_deallocated_no_reuse_stream++;
    cache.m_deallocated_no_reuse_stream_noc_control_message += nocCtrl;
    cache.m_deallocated_no_reuse_stream_noc_control_evict_message +=
        nocCtrlEvict;
    cache.m_deallocated_no_reuse_stream_noc_data_message += nocData;
  }
}

void AbstractStreamAwareController::recordLLCReqQueueStats(
    const RequestStatisticPtr &reqStat, const DynamicStreamSliceIdVec &sliceIds,
    bool isLoad) {
  this->recordPCReq(reqStat);
  if (sliceIds.isValid()) {
    // An LLC stream request.
    ++m_statLLCStreamReq;
    if (sliceIds.sliceIds.size() > 1) {
      // Multicast LLC stream request.
      ++m_statLLCMulticastStreamReq;
    }
  } else {
    // A core request.
    ++m_statCoreReq;
    if (isLoad) {
      ++m_statCoreLoadReq;
    }
    if (reqStat && reqStat->isStream) {
      // A normal stream req from core.
      ++m_statCoreStreamReq;
      if (isLoad) {
        ++m_statCoreStreamLoadReq;
      }
    }
  }
}