#include "AbstractStreamAwareController.hh"

#include "mem/ruby/network/garnet2.0/GarnetNetwork.hh"
#include "sim/stream_nuca/stream_nuca_map.hh"

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
      mlcStreamBufferInitNumEntries(p->mlc_stream_buffer_init_num_entries) {
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

void AbstractStreamAwareController::init() {

  AbstractController::init();

  /**
   * Register myself to StreamNUCAMap. This can not be done in the constructor
   * as the MachineId is initialized in the derived class constructor.
   */
  assert(myParams->addr_ranges.size() == 1 && "Multiple AddrRanges.");
  StreamNUCAMap::addNonUniformNode(myParams->router_id, this->m_machineID,
                                   myParams->addr_ranges.front(),
                                   myParams->numa_banks);
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
  m_statLLCScheduledComputeMicroOps
      .name(name() + ".llcScheduledStreamComputeMicroOps")
      .desc("number of llc stream computation microops scheduled")
      .flags(Stats::nozero);

#define complete_micro_op(Addr, Compute)                                       \
  m_statLLCScheduled##Addr##Compute##MicroOps                                  \
      .name(name() + ".llcScheduledStream" #Addr #Compute "MicroOps")          \
      .desc("number of llc stream " #Addr #Compute "microops scheduled")       \
      .flags(Stats::nozero)

  complete_micro_op(Affine, LoadCompute);
  complete_micro_op(Affine, StoreCompute);
  complete_micro_op(Affine, AtomicCompute);
  complete_micro_op(Affine, Reduce);
  complete_micro_op(Affine, Update);
  complete_micro_op(Indirect, LoadCompute);
  complete_micro_op(Indirect, StoreCompute);
  complete_micro_op(Indirect, AtomicCompute);
  complete_micro_op(Indirect, Reduce);
  complete_micro_op(Indirect, Update);
  complete_micro_op(PointerChase, LoadCompute);
  complete_micro_op(PointerChase, StoreCompute);
  complete_micro_op(PointerChase, AtomicCompute);
  complete_micro_op(PointerChase, Reduce);
  complete_micro_op(PointerChase, Update);
  complete_micro_op(MultiAffine, LoadCompute);
  complete_micro_op(MultiAffine, StoreCompute);
  complete_micro_op(MultiAffine, AtomicCompute);
  complete_micro_op(MultiAffine, Reduce);
  complete_micro_op(MultiAffine, Update);
#undef complete_micro_op

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
  m_statLLCRealXAWConflictAtomics
      .name(name() + ".llcStreamAtomicsRealXAWConflict")
      .desc("number of llc stream atomics that has real X-after-write conflict")
      .flags(Stats::nozero);
  m_statLLCDeadlockAtomics.name(name() + ".llcStreamAtomicsDeadlock")
      .desc("number of llc stream atomics that triggers deadlock")
      .flags(Stats::nozero);

  m_statLLCNumDirectStreams.init(1, 32, 2)
      .name(name() + ".llcNumDirectStreams")
      .desc("Sample of number of LLC direct streams.")
      .flags(Stats::pdf)
      .flags(Stats::nozero);

  m_statLLCNumInflyComputations
      .init(1, myParams->llc_stream_engine_max_infly_computation, 1)
      .name(name() + ".llcNumInflyComputations")
      .desc("Sample of number of LLC computation infly.")
      .flags(Stats::pdf)
      .flags(Stats::nozero);
  m_statLLCNumReadyComputations.init(1, 32, 1)
      .name(name() + ".llcNumReadyComputations")
      .desc("Sample of number of LLC computation ready but not infly.")
      .flags(Stats::pdf)
      .flags(Stats::nozero);

  // Register stats callback.
  Stats::registerResetCallback(
      new MakeCallback<PCRequestRecorder, &PCRequestRecorder::reset>(
          &this->pcReqRecorder, true /* auto delete */));
  Stats::registerDumpCallback(
      new MakeCallback<PCRequestRecorder, &PCRequestRecorder::dump>(
          &this->pcReqRecorder, true /* auto delete */));
}

MachineID
AbstractStreamAwareController::mapAddressToLLCOrMem(Addr addr,
                                                    MachineType mtype) const {
  // Ideally we should check mtype to be LLC or directory, etc.
  // But here I ignore it.
  if (mtype == MachineType::MachineType_L2Cache) {
    return mapAddressToRange(addr, mtype, this->llcSelectLowBit,
                             this->llcSelectNumBits, 0 /* cluster_id. */
    );
  } else if (mtype == MachineType::MachineType_Directory) {
    return this->mapAddressToMachine(addr, mtype);
  } else {
    panic("MachineType should only be L2 or Dir, got %s.",
          MachineType_to_string(mtype));
  }
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
    const RequestStatisticPtr &reqStat, const DynStreamSliceIdVec &sliceIds,
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

int AbstractStreamAwareController::getNumRows() const {
  auto network = this->m_net_ptr;
  auto garnet = dynamic_cast<GarnetNetwork *>(network);
  if (!garnet) {
    panic("Only works with Garnet to get NumRows/NumCols.");
  }
  return garnet->getNumRows();
}
int AbstractStreamAwareController::getNumCols() const {
  auto network = this->m_net_ptr;
  auto garnet = dynamic_cast<GarnetNetwork *>(network);
  if (!garnet) {
    panic("Only works with Garnet to get NumRows/NumCols.");
  }
  return garnet->getNumCols();
}

bool AbstractStreamAwareController::isMyNeighbor(MachineID machineId) const {
  auto cols = this->getNumCols();
  auto myId = this->getMachineID().getNum();
  int myRow = myId / cols;
  int myCol = myId % cols;
  int row = machineId.getNum() / cols;
  int col = machineId.getNum() % cols;
  return std::abs(myRow - row) + std::abs(myCol - col) == 1;
}

Cycles AbstractStreamAwareController::adjustResponseLat(Cycles responseLat,
                                                        Addr paddr) const {
  auto range = StreamNUCAMap::getRangeMapContaining(paddr);
  if (!range || !range->isStreamPUM) {
    return responseLat;
  }
  // Charge the number of wordlines as the latency.
  return Cycles(range->elementBits);
}