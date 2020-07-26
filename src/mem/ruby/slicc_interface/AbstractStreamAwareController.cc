#include "AbstractStreamAwareController.hh"

#include "RubySlicc_ComponentMapping.hh"

AbstractStreamAwareController::GlobalMap
    AbstractStreamAwareController::globalMap;
std::list<AbstractStreamAwareController *>
    AbstractStreamAwareController::globalList;

AbstractStreamAwareController::AbstractStreamAwareController(const Params *p)
    : AbstractController(p), myParams(p),
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
  m_statCoreStreamReq.name(name() + ".coreStreamRequests")
      .desc("number of core stream requests seen")
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
      hack("Register %s.\n", machineId);
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
    cache.m_deallocated_no_reuse_stream_noc_control_evict_message += nocCtrlEvict;
    cache.m_deallocated_no_reuse_stream_noc_data_message += nocData;
  }
}

void AbstractStreamAwareController::recordLLCReqQueueStats(
    const RequestStatisticPtr &reqStat,
    const DynamicStreamSliceIdVec &sliceIds) {
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
    if (reqStat && reqStat->isStream) {
      // A normal stream req from core.
      ++m_statCoreStreamReq;
    }
  }
}