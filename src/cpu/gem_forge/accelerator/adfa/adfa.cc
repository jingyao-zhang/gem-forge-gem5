#include "adfa.hh"

#include "base/trace.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"
#include "cpu/gem_forge/llvm_trace_cpu_delegator.hh"
#include "debug/AbstractDataFlowAccelerator.hh"

AbstractDataFlowCore::AbstractDataFlowCore(
    const std::string &_id, LLVMTraceCPU *_cpu,
    GemForgeCPUDelegator *_cpuDelegator,
    AbstractDataFlowAcceleratorParams *params)
    : id(_id), cpu(_cpu), cpuDelegator(_cpuDelegator), busy(false),
      dataFlow(nullptr), issueWidth(16), fetchQueueSize(64), robSize(512) {
  this->issueWidth = params->adfaCoreIssueWidth;
  this->enableSpeculation = params->adfaEnableSpeculation;
  this->breakIVDep = params->adfaBreakIVDep;
  this->breakRVDep = params->adfaBreakRVDep;
  this->breakUnrollableControlDep = params->adfaBreakUnrollableControlDep;
  this->idealMem = params->adfaIdealMem;
  this->numBanks = params->adfaNumBanks;
  this->numPortsPerBank = params->adfaNumPortsPerBank;

  this->bankManager = new BankManager(this->cpuDelegator->cacheLineSize(),
                                      this->numBanks, this->numPortsPerBank);
}

AbstractDataFlowCore::~AbstractDataFlowCore() {
  if (this->bankManager != nullptr) {
    delete this->bankManager;
    this->bankManager = nullptr;
  }
}

void AbstractDataFlowCore::dump() {
  inform("ADFCore %s: Committed insts %f.\n", this->name(),
         this->numCommittedInst.value());
  inform("ADFCore %s: FetchQueue ======================================\n",
         this->name());
  for (const auto &instId : this->fetchQueue) {
    const auto inst = this->inflyInstMap.at(instId);
    inst->dumpBasic();
  }
  inform("ADFCore %s: FetchQueue End ==================================\n",
         this->name());
  inform("ADFCore %s: ROB ======================================\n",
         this->name());
  size_t robIdx = 0;
  for (const auto &instId : this->rob) {
    const auto inst = this->inflyInstMap.at(instId);
    if (robIdx == 0) {
      inform("ROB Head %d.", this->inflyInstStatus.at(instId));
      robIdx++;
    }
    inst->dumpBasic();
  }
  inform("ADFCore %s: ROB End ==================================\n",
         this->name());
}

void AbstractDataFlowCore::regStats() {
  this->numIssuedDist.init(0, this->issueWidth, 1)
      .name(this->id + ".issued_per_cycle")
      .desc("Number of inst issued each cycle")
      .flags(Stats::pdf);
  this->numIssuedLoadDist.init(0, this->numBanks * this->numPortsPerBank, 1)
      .name(this->id + ".issued_load_per_cycle")
      .desc("Number of inst issued loads each cycle")
      .flags(Stats::pdf);
  this->numCommittedDist.init(0, 8, 1)
      .name(this->id + ".adfa.committed_per_cycle")
      .desc("Number of insts committed each cycle")
      .flags(Stats::pdf);
  this->numExecution.name(this->id + ".numExecution")
      .desc("Number of times ADFA get executed")
      .prereq(this->numExecution);
  this->numCycles.name(this->id + ".numCycles")
      .desc("Number of cycles ADFA is running")
      .prereq(this->numCycles);
  this->numCommittedInst.name(this->id + ".numCommittedInst")
      .desc("Number of insts ADFA committed")
      .prereq(this->numCommittedInst);
  this->numBankConflicts.name(this->id + ".numBankConflicts")
      .desc("Number of insts ADFA causing bank conflicts")
      .prereq(this->numBankConflicts);
}

void AbstractDataFlowCore::start(DynamicInstructionStreamInterface *dataFlow) {
  if (this->isBusy()) {
    panic("Start the core while it's still busy.");
  }
  this->busy = true;
  this->dataFlow = dataFlow;

  this->currentAge = 0;
  this->inflyInstAge.clear();
  this->inflyInstStatus.clear();
  this->inflyInstMap.clear();
  this->rob.clear();
  this->readyInsts.clear();
  this->numExecution++;

  DPRINTF(AbstractDataFlowAccelerator, "ADFA: start execution.\n");
}

void AbstractDataFlowCore::tick() {
  if (!this->isBusy()) {
    return;
  }

  this->fetch();
  this->decode();
  this->markReady();
  this->issue();
  this->commit();
  this->release();
  this->numCycles++;

  if (this->dataFlow->hasEnded() && this->inflyInstMap.empty()) {
    // Mark that we are done.
    this->busy = false;
    hack("We are done.\n");
    DPRINTF(AbstractDataFlowAccelerator, "Work done.\n");
  }
}

void AbstractDataFlowCore::fetch() {
  if (this->dataFlow->hasEnded()) {
    return;
  }

  // We maintain a crazy huge rob size.
  if (this->fetchQueue.size() >= this->fetchQueueSize) {
    return;
  }

  while (this->fetchQueue.size() < this->fetchQueueSize) {
    auto inst = this->dataFlow->fetch();
    if (inst == nullptr) {
      // We have just reached the end of the data flow.
      break;
    }

    auto id = inst->getId();
    this->fetchQueue.push_back(id);
    this->inflyInstAge.emplace(id, this->currentAge++);
    this->inflyInstStatus.emplace(id, InstStatus::FETCHED);
    this->inflyInstMap.emplace(id, inst);
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: fetched inst %u.\n", id);
  }
}

void AbstractDataFlowCore::decode() {
  while (this->rob.size() < this->robSize && !this->fetchQueue.empty()) {
    auto id = this->fetchQueue.front();
    auto inst = this->inflyInstMap.at(id);

    if (!inst->canDispatch(cpu)) {
      break;
    }

    // Hook to any instruction specific dispatch operation.
    // ! Note that ADFA core does not have LSQ.
    inst->dispatch(cpu);

    // We update RegionStats here.
    // ! Now RegionStats comes from ThreadContext. Here I assume it
    // ! always from CPU's main thread. This will certainly break when
    // ! TLS enabled (at least for the inner most region that is
    // ! distributed among ADFA cores).
    // const auto &TDG = inst->getTDG();
    if (inst->getTDG().bb() != 0) {
      auto mainThread = cpu->getMainThread();
      assert(mainThread && "Failed to get main thread.");
      auto regionStats = mainThread->getRegionStats();
      if (regionStats) {
        regionStats->update(inst->getTDG().bb());
      }
    }

    // Able to decode.
    this->fetchQueue.pop_front();
    this->rob.push_back(id);
    this->inflyInstStatus.at(id) = InstStatus::DECODED;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: decoded inst %u.\n", id);
  }
}

void AbstractDataFlowCore::markReady() {
  /**
   * This is quite inefficient as we loop through all the rob.
   */
  for (auto id : this->rob) {
    if (this->inflyInstStatus.at(id) != InstStatus::DECODED) {
      continue;
    }

    // Manually check the dependence, without using inst's isDependenceReady
    // function.
    bool ready = true;
    auto inst = this->inflyInstMap.at(id);
    for (const auto &dep : inst->getTDG().deps()) {
      bool shouldCheck = true;
      switch (dep.type()) {
      case ::LLVM::TDG::TDGInstructionDependence::CONTROL: {
        shouldCheck = false;
        break;
      }
      case ::LLVM::TDG::TDGInstructionDependence::POST_DOMINANCE_FRONTIER: {
        if (this->enableSpeculation) {
          shouldCheck = false;
        }
        break;
      }
      case ::LLVM::TDG::TDGInstructionDependence::UNROLLABLE_CONTROL: {
        if (this->enableSpeculation || this->breakUnrollableControlDep) {
          shouldCheck = false;
        }
        break;
      }
      case ::LLVM::TDG::TDGInstructionDependence::INDUCTION_VARIABLE: {
        if (this->breakIVDep) {
          shouldCheck = false;
        }
        break;
      }
      case ::LLVM::TDG::TDGInstructionDependence::REDUCTION_VARIABLE: {
        if (this->breakRVDep) {
          shouldCheck = false;
        }
        break;
      }
      case ::LLVM::TDG::TDGInstructionDependence::STREAM: {
        // Stream dependence is not checked here.
        shouldCheck = false;
        break;
      }
      default: {
        // For other type of dependence, we need to enforce.
        break;
      }
      }
      if (shouldCheck) {
        const auto depId = dep.dependent_id();
        auto statusIter = this->inflyInstStatus.find(depId);
        if (statusIter != this->inflyInstStatus.end() &&
            statusIter->second != InstStatus::FINISHED) {
          ready = false;
          break;
        }
      }
    }
    if (inst->hasStreamUse()) {
      StreamEngine::StreamUserArgs args(inst->getSeqNum(), inst->getPC(),
                                        inst->usedStreamIds);
      auto SE = cpu->getAcceleratorManager()->getStreamEngine();
      if (!SE->areUsedStreamsReady(args)) {
        ready = false;
        if (id == 11489479) {
          hack("Due to stream reason it is not ready.\n");
        }
      }
    }
    if (ready) {
      DPRINTF(AbstractDataFlowAccelerator,
              "ADFA: mark ready inst %u, current ready list size %u.\n", id,
              this->readyInsts.size());
      this->inflyInstStatus.at(id) = InstStatus::READY;
      auto age = this->inflyInstAge.at(id);
      auto readyIter = this->readyInsts.begin();
      auto readyEnd = this->readyInsts.end();
      bool inserted = false;
      while (readyIter != readyEnd) {
        auto readyId = *readyIter;
        if (this->inflyInstAge.at(readyId) > age) {
          this->readyInsts.insert(readyIter, id);
          inserted = true;
          break;
        }
        ++readyIter;
      }
      if (!inserted) {
        this->readyInsts.push_back(id);
      }
    }
  }
}

void AbstractDataFlowCore::issue() {
  size_t issued = 0;
  size_t issuedLoad = 0;

  // Clear the bank manager for this cycle.
  this->bankManager->clear();

  auto readyIter = this->readyInsts.begin();
  auto readyEnd = this->readyInsts.end();
  while (readyIter != readyEnd) {
    // Some issue width.
    if (issued >= this->issueWidth) {
      break;
    }
    // Enforce banked cache confliction.
    auto id = *readyIter;
    auto inst = this->inflyInstMap.at(id);

    const auto &TDG = inst->getTDG();
    if (TDG.has_load() || TDG.has_store()) {

      // Never issue memory request if the port is already blocked.
      if (cpu->dataPort.isBlocked()) {
        readyIter++;
        DPRINTF(AbstractDataFlowAccelerator, "ADFA: Blocked mem inst %u.\n",
                id);
        continue;
      }

      auto addr = TDG.has_load() ? TDG.load().addr() : TDG.store().addr();
      auto size = TDG.has_load() ? TDG.load().size() : TDG.store().size();
      // For now just look at the first cache line.
      auto cacheLineSize = this->cpuDelegator->cacheLineSize();
      size = std::min(size, cacheLineSize - (addr % cacheLineSize));
      if (!this->bankManager->isNonConflict(addr, size)) {
        // Has conflict, not issue this one.
        readyIter++;
        this->numBankConflicts++;
        continue;
      } else {
        // No conflict happen, good to go.
        this->bankManager->access(addr, size);
      }
    }

    // Ready to go.
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: issue inst %u.\n", id);

    if (TDG.has_load() || TDG.has_store()) {
      if (this->idealMem) {
        // Special case for ideal memory: add to the completion queue.
        auto completeTick = cpu->clockEdge(Cycles(this->idealMemLatency));
        this->idealMemCompleteQueue.emplace_back(completeTick, id);
      } else {

        DPRINTF(AbstractDataFlowAccelerator, "ADFA: execute load inst %u.\n",
                id);
        inst->execute(cpu);
        DPRINTF(AbstractDataFlowAccelerator, "ADFA: executed load inst %u.\n",
                id);
        // For store instruction, we write back immediately as we have all the
        // memory/control dependence resolved.
        if (inst->isStoreInst()) {
          inst->writeback(cpu);
        }
      }
    } else {
      // Non-memory instructions.
      inst->execute(cpu);
    }
    ++issued;
    if (TDG.has_load()) {
      ++issuedLoad;
    }
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: check inflyInstStatus %u.\n",
            id);
    this->inflyInstStatus.at(id) = InstStatus::ISSUED;
    readyIter = this->readyInsts.erase(readyIter);
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: issued inst %u.\n", id);
  }

  this->numIssuedDist.sample(issued);
  this->numIssuedLoadDist.sample(issuedLoad);
}

void AbstractDataFlowCore::commit() {

  // First we check the idealMemCompleteQueue.
  if (this->idealMem) {
    auto iter = this->idealMemCompleteQueue.begin();
    auto end = this->idealMemCompleteQueue.end();
    auto currentTick = cpuDelegator->cyclesToTicks(cpuDelegator->curCycle());
    while (iter != end) {
      if (iter->first > currentTick) {
        break;
      }
      // Time to mark it complete.
      auto id = iter->second;
      DPRINTF(AbstractDataFlowAccelerator, "ADFA: inst %lu finished.\n", id);
      assert(this->inflyInstStatus.at(id) == InstStatus::ISSUED);
      this->inflyInstStatus.at(id) = InstStatus::FINISHED;
      iter = this->idealMemCompleteQueue.erase(iter);
    }
  }

  for (auto id : this->rob) {
    auto inst = this->inflyInstMap.at(id);
    if (this->inflyInstStatus.at(id) == InstStatus::ISSUED) {

      // Ideal memory mode, memory instructions are handled above.
      if (this->idealMem && (inst->isLoadInst() || inst->isStoreInst())) {
        continue;
      }

      bool done = inst->isCompleted();
      if (inst->isStoreInst()) {
        done &= inst->isWritebacked();
      }
      if (done) {
        DPRINTF(AbstractDataFlowAccelerator, "ADFA: inst %lu finished.\n", id);
        this->inflyInstStatus.at(id) = InstStatus::FINISHED;
        continue;
      }
    }
    inst->tick();
  }
}

void AbstractDataFlowCore::release() {
  // release in order.
  unsigned committed = 0;
  for (auto iter = this->rob.begin(), end = this->rob.end(); iter != end;) {
    auto id = *iter;
    if (this->inflyInstStatus.at(id) != InstStatus::FINISHED) {
      break;
    }
    /**
     * ! This is really a bad design, but I have to let the instruction know
     * ! it is committed.
     */
    this->inflyInstMap.at(id)->commit(cpu);
    ++committed;
    iter = this->rob.erase(iter);
    auto inst = this->inflyInstMap.at(id);
    this->inflyInstStatus.erase(id);
    this->inflyInstAge.erase(id);
    this->inflyInstMap.erase(id);
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: inst %lu release.\n", id);
    this->dataFlow->commit(inst);
  }
  this->numCommittedInst += committed;
  this->numCommittedDist.sample(committed);
}

AbstractDataFlowAccelerator::AbstractDataFlowAccelerator(Params *_params)
    : GemForgeAccelerator(_params), params(_params), handling(NONE),
      dataFlow(nullptr) {
  this->numCores = this->params->adfaNumCores;
  this->enableTLS = this->params->adfaEnableTLS;
}
AbstractDataFlowAccelerator::~AbstractDataFlowAccelerator() {
  if (this->dataFlow != nullptr) {
    delete this->dataFlowDispatcher;
    this->dataFlowDispatcher = nullptr;
    delete this->dataFlow;
    this->dataFlow = nullptr;
  }
  for (auto &core : this->cores) {
    delete core;
    core = nullptr;
  }
}

void AbstractDataFlowAccelerator::handshake(
    GemForgeCPUDelegator *_cpuDelegator, GemForgeAcceleratorManager *_manager) {

  GemForgeAccelerator::handshake(_cpuDelegator, _manager);

  LLVMTraceCPU *_cpu = nullptr;
  if (auto llvmTraceCPUDelegator =
          dynamic_cast<LLVMTraceCPUDelegator *>(_cpuDelegator)) {
    _cpu = llvmTraceCPUDelegator->cpu;
  }
  assert(_cpu != nullptr && "Only work for LLVMTraceCPU so far.");
  this->cpu = _cpu;

  /**
   * Be careful to reserve space so that core is not moved.
   * It can not be moved as it contains stats.
   */
  for (int i = 0; i < this->numCores; ++i) {
    auto id = this->manager->name() + ".adfa.core" + std::to_string(i);
    this->cores.push_back(
        new AbstractDataFlowCore(id, _cpu, _cpuDelegator, this->params));
  }
}

void AbstractDataFlowAccelerator::regStats() {
  GemForgeAccelerator::regStats();
  this->numConfigured.name(this->manager->name() + ".adfa.numConfigured")
      .desc("Number of times ADFA get configured")
      .prereq(this->numConfigured);
  this->numExecution.name(this->manager->name() + ".adfa.numExecution")
      .desc("Number of times ADFA get executed")
      .prereq(this->numExecution);
  this->numCycles.name(this->manager->name() + ".adfa.numCycles")
      .desc("Number of cycles ADFA is running")
      .prereq(this->numCycles);
  this->numTLSJobs.name(this->manager->name() + ".adfa.numTLSJobs")
      .desc("Number of TLS jobs ADFA run")
      .prereq(this->numTLSJobs);
  this->numTLSJobsSerialized
      .name(this->manager->name() + ".adfa.numTLSJobsSerialized")
      .desc("Number of TLS jobs ADFA serialized")
      .prereq(this->numTLSJobsSerialized);

  for (auto &core : this->cores) {
    core->regStats();
  }
}

bool AbstractDataFlowAccelerator::handle(LLVMDynamicInst *inst) {
  if (this->handling != NONE) {
    panic("ADFA is already busy, can not handle other adfa instruction.");
  }
  if (auto ConfigInst = dynamic_cast<ADFAConfigInst *>(inst)) {
    this->handling = CONFIG;
    this->currentInst.config = ConfigInst;
    this->configOverheadInCycles = 10;

    // Store the loop start pc.
    this->configuredLoopStartPC = inst->getTDG().adfa_config().start_pc();
    this->configuredLoopName = inst->getTDG().adfa_config().region();

    // Simply open the data flow stream.
    if (this->dataFlow == nullptr) {
      auto dataFlowFileName = cpu->getTraceFolder() + "/" +
                              inst->getTDG().adfa_config().data_flow();
      this->dataFlowDispatcher =
          new DynamicInstructionStreamDispatcher(dataFlowFileName);
      this->dataFlow = new DynamicInstructionStream(
          this->dataFlowDispatcher->getMainBuffer());
    }
    this->numConfigured++;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start configure.\n");

    this->manager->scheduleTickNextCycle();
    return true;

  } else if (auto StartInst = dynamic_cast<ADFAStartInst *>(inst)) {
    this->handling = START;
    this->currentInst.start = StartInst;
    this->numExecution++;

    inform("ADFA: start region %s.\n", this->configuredLoopName.c_str());

    // Take a checkpoints.
    // cpu->getRegionStats()->checkpoint(this->configuredLoopName);

    // Create the dataflow.
    assert(this->dataFlow == nullptr && "Dataflow already created.");
    this->dataFlow = new DynamicInstructionStream(StartInst->getBuffer());

    if (this->enableTLS) {
      // TLS mode.
      this->TLSLHSIter = this->dataFlow->fetchIter();
      this->TLSJobId = 0;
      this->createTLSJobs();
    } else {
      // Non-TLS mode.
      // Create a new job in the queue
      this->pendingJobs.emplace_back();
      auto &newJob = this->pendingJobs.back();
      newJob.dataFlow = new DynamicInstructionStreamInterfaceConditionalEnd(
          this->dataFlow, [](LLVMDynamicInst *inst) -> bool {
            return inst->getInstName() == "df-end";
          });
    }

    this->manager->scheduleTickNextCycle();
    return true;
  }
  return false;
}

void AbstractDataFlowAccelerator::dump() {
  for (auto &core : this->cores) {
    core->dump();
  }
}

void AbstractDataFlowAccelerator::tick() {
  switch (this->handling) {
  case NONE: {
    return;
  }
  case CONFIG: {
    this->numCycles++;
    this->tickConfig();
    this->manager->scheduleTickNextCycle();
    break;
  }
  case START: {
    this->numCycles++;
    this->tickStart();
    this->manager->scheduleTickNextCycle();
    break;
  }
  default: {
    panic("Unknown handling instruction.");
  }
  }
}

void AbstractDataFlowAccelerator::tickConfig() {
  this->configOverheadInCycles--;
  if (this->configOverheadInCycles == 0) {
    this->currentInst.config->markFinished();
    this->currentInst.config = nullptr;
    this->handling = NONE;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start configure: DONE.\n");
  }
}

void AbstractDataFlowAccelerator::tickStart() {
  // Try to get new jobs.
  if (this->enableTLS) {
    this->createTLSJobs();
  }

  // Try to schedule new jobs.
  for (auto &core : this->cores) {
    if (this->pendingJobs.empty()) {
      break;
    }
    if (core->isBusy()) {
      continue;
    }

    auto &pendingJob = this->pendingJobs.front();
    if (pendingJob.shouldSerialize && !this->workingJobs.empty()) {
      // Can not issue this job as we have to serialize.
      break;
    }

    pendingJob.core = core;
    core->start(pendingJob.dataFlow);
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: Start pending job %lu.\n",
            pendingJob.jobId);
    this->workingJobs.push_back(pendingJob);
    this->pendingJobs.pop_front();
  }

  // Tick.
  for (auto &core : this->cores) {
    core->tick();
  }

  // Try to collect finished jobs.
  while (!this->workingJobs.empty()) {
    auto &workingJob = this->workingJobs.front();
    if (workingJob.core->isBusy()) {
      // Still busy.
      break;
    }
    // The core is done with the job.
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: Finish working job %lu.\n",
            workingJob.jobId);

    workingJob.core = nullptr;
    delete workingJob.dataFlow;
    workingJob.dataFlow = nullptr;
    this->workingJobs.pop_front();
  }

  if (this->workingJobs.empty() && this->pendingJobs.empty()) {
    // We are done for all jobs. Simply mark the instruction finished for now.
    this->currentInst.start->markFinished();
    this->currentInst.start = nullptr;
    this->handling = NONE;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start execution: DONE.\n");
    inform("ADFA: end region %s.\n", this->configuredLoopName.c_str());

    // For TLS mode, remember to release the endToken.
    if (this->enableTLS) {
      if ((*this->TLSLHSIter)->inst->getInstName() != "df-end") {
        panic("The last token should be endToken for tls mode.");
      }
      this->dataFlow->commit(this->TLSLHSIter);
    }

    // Release the dataFlow.
    delete this->dataFlow;
    this->dataFlow = nullptr;

    // Take a checkpoint.
    // cpu->getRegionStats()->checkpoint(this->configuredLoopName);
  }
}

void AbstractDataFlowAccelerator::createTLSJobs() {
  while (this->pendingJobs.size() <= this->cores.size()) {
    if ((*this->TLSLHSIter)->inst->getInstName() == "df-end") {
      // We have reached the end of the stream.
      return;
    }
    Job newJob;
    newJob.jobId = this->TLSJobId++;
    newJob.instIds = std::make_shared<std::unordered_set<LLVMDynamicInstId>>();

    auto TLSRHSIter = this->TLSLHSIter;

    do {
      // Try to detect inter-iteration dependences.
      if (!newJob.shouldSerialize) {
        newJob.shouldSerialize = this->hasTLSDependence((*TLSRHSIter)->inst);
      }

      // Add to our instIds set.
      newJob.instIds->insert((*TLSRHSIter)->inst->getId());

      TLSRHSIter = this->dataFlow->fetchIter();

    } while (!this->isTLSBoundary((*TLSRHSIter)->inst));

    // Creating the dataFlow.
    newJob.dataFlow = new DynamicInstructionStreamInterfaceFixedEnd(
        this->dataFlow, this->TLSLHSIter, TLSRHSIter);

    DPRINTF(AbstractDataFlowAccelerator,
            "ADFA: Create TLS job %lu, insts %lu.\n", newJob.jobId,
            newJob.instIds->size());
    this->numTLSJobs++;
    if (newJob.shouldSerialize) {
      this->numTLSJobsSerialized++;
    }

    this->pendingJobs.push_back(newJob);
    // Update our LHSIter to detect next iteration.
    this->TLSLHSIter = TLSRHSIter;
  }
}

bool AbstractDataFlowAccelerator::isTLSBoundary(LLVMDynamicInst *inst) const {
  if (inst->getInstName() == "df-end") {
    return true;
  }
  if (inst->getTDG().pc() == this->configuredLoopStartPC) {
    return true;
  }
  return false;
}

bool AbstractDataFlowAccelerator::hasTLSDependence(
    LLVMDynamicInst *inst) const {
  // First check the working jobs.
  for (const auto &job : this->workingJobs) {
    if (job.instIds == nullptr) {
      panic("Job should have instIds for TLS job.");
    }
    for (const auto &dep : inst->getTDG().deps()) {
      // We only check for memory dependence.
      if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::MEMORY) {
        const auto depId = dep.dependent_id();
        if (job.instIds->count(depId) != 0) {
          return true;
        }
      }
    }
  }
  // Do the same for pending jobs.
  for (const auto &job : this->pendingJobs) {
    if (job.instIds == nullptr) {
      panic("Job should have instIds for TLS job.");
    }
    for (const auto &dep : inst->getTDG().deps()) {
      // We only check for memory dependence.
      if (dep.type() == ::LLVM::TDG::TDGInstructionDependence::MEMORY) {
        const auto depId = dep.dependent_id();
        if (job.instIds->count(depId) != 0) {
          return true;
        }
      }
    }
  }
  return false;
}

AbstractDataFlowAccelerator *AbstractDataFlowAcceleratorParams::create() {
  return new AbstractDataFlowAccelerator(this);
}