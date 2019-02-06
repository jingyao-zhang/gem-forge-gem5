#include "adfa.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "cpu/llvm_trace/dyn_inst_stream.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/AbstractDataFlowAccelerator.hh"

AbstractDataFlowCore::AbstractDataFlowCore(const std::string &_id,
                                           LLVMTraceCPU *_cpu)
    : id(_id),
      cpu(_cpu),
      busy(false),
      dataFlow(nullptr),
      issueWidth(16),
      robSize(512) {
  auto cpuParams = dynamic_cast<const LLVMTraceCPUParams *>(_cpu->params());
  this->enableSpeculation = cpuParams->adfaEnableSpeculation;
  this->breakIVDep = cpuParams->adfaBreakIVDep;
  this->breakRVDep = cpuParams->adfaBreakRVDep;
  this->numBanks = cpuParams->adfaNumBanks;
  this->numPortsPerBank = cpuParams->adfaNumPortsPerBank;

  this->bankManager = new BankManager(this->cpu->system->cacheLineSize(),
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
}

void AbstractDataFlowCore::regStats() {
  this->numIssuedDist.init(0, this->issueWidth, 1)
      .name(this->id + ".issued_per_cycle")
      .desc("Number of inst issued each cycle")
      .flags(Stats::pdf);
  this->numCommittedDist.init(0, 8, 1)
      .name(this->id + ".adfa.committed_per_cycle")
      .desc("Number of insts committed each cycle")
      .flags(Stats::pdf);
  this->numConfigured.name(this->id + ".numConfigured")
      .desc("Number of times ADFA get configured")
      .prereq(this->numConfigured);
  this->numExecution.name(this->id + ".numExecution")
      .desc("Number of times ADFA get executed")
      .prereq(this->numExecution);
  this->numCycles.name(this->id + ".numCycles")
      .desc("Number of cycles ADFA is running")
      .prereq(this->numCycles);
  this->numCommittedInst.name(this->id + ".numCommittedInst")
      .desc("Number of insts ADFA committed")
      .prereq(this->numCommittedInst);
}

void AbstractDataFlowCore::start(DynamicInstructionStream *dataFlow) {
  if (this->isBusy()) {
    panic("Start the core while it's still busy.");
  }
  this->busy = true;
  this->dataFlow = dataFlow;

  this->endToken = nullptr;
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
  this->markReady();
  this->issue();
  this->commit();
  this->release();

  if (this->endToken != nullptr && this->rob.empty()) {
    this->dataFlow->commit(this->endToken);
    this->endToken = nullptr;

    // Mark that we are done.
    this->busy = false;
  }
}

void AbstractDataFlowCore::fetch() {
  if (this->endToken != nullptr) {
    return;
  }

  // We maintain a crazy huge rob size.
  if (this->rob.size() >= this->robSize) {
    return;
  }

  while (this->dataFlow->fetchSize() > 0 && this->rob.size() < this->robSize) {
    auto inst = this->dataFlow->fetch();
    if (inst->getInstName() == "df-end") {
      // We have encountered the end token.
      this->endToken = inst;

      DPRINTF(AbstractDataFlowAccelerator, "ADFA: fetched end token %u.\n",
              inst->getId());
      break;
    }
    // This is a normal instruction, insert into rob.

    // We update RegionStats here.
    const auto &TDG = inst->getTDG();
    if (TDG.bb() != 0) {
      cpu->updateBasicBlock(TDG.bb());
    }

    auto id = inst->getId();
    this->rob.push_back(id);
    this->inflyInstAge.emplace(id, this->currentAge++);
    this->inflyInstStatus.emplace(id, InstStatus::FETCHED);
    this->inflyInstMap.emplace(id, inst);
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: fetched inst %u.\n", id);
  }
}

void AbstractDataFlowCore::markReady() {
  /**
   * This is quite inefficient as we loop through all the rob.
   */
  for (auto id : this->rob) {
    if (this->inflyInstStatus.at(id) != InstStatus::FETCHED) {
      continue;
    }

    // Manually check the dependence, without using inst's isDependenceReady
    // function.
    bool ready = true;
    auto inst = this->inflyInstMap.at(id);
    for (const auto &dep : inst->getTDG().deps()) {
      bool shouldCheck = true;
      if (dep.type() ==
          ::LLVM::TDG::TDGInstructionDependence::POST_DOMINANCE_FRONTIER) {
        if (this->enableSpeculation) {
          continue;
        }
      }
      if (dep.type() ==
          ::LLVM::TDG::TDGInstructionDependence::INDUCTION_VARIABLE) {
        if (this->breakIVDep) {
          continue;
        }
      }
      if (dep.type() ==
          ::LLVM::TDG::TDGInstructionDependence::REDUCTION_VARIABLE) {
        if (this->breakRVDep) {
          continue;
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

  // Clear the bank manager for this cycle.
  this->bankManager->clear();

  auto readyIter = this->readyInsts.begin();
  auto readyEnd = this->readyInsts.end();
  while (readyIter != readyEnd) {
    // Some issue width.
    if (issued > this->issueWidth) {
      break;
    }
    // Enforce banked cache confliction.
    auto id = *readyIter;
    auto inst = this->inflyInstMap.at(id);

    const auto &TDG = inst->getTDG();
    if (TDG.has_load() || TDG.has_store()) {
      auto addr = TDG.has_load() ? TDG.load().addr() : TDG.store().addr();
      auto size = TDG.has_load() ? TDG.load().size() : TDG.store().size();
      // For now just look at the first cache line.
      auto cacheLineSize = this->cpu->system->cacheLineSize();
      size = std::min(size, cacheLineSize - (addr % cacheLineSize));
      if (!this->bankManager->isNonConflict(addr, size)) {
        // Has conflict, not issue this one.
        readyIter++;
        continue;
      } else {
        // No conflict happen, good to go.
        this->bankManager->access(addr, size);
      }
    }

    // Ready to go.
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: issue inst %u.\n", id);
    inst->execute(cpu);
    // For store instruction, we write back immediately as we have all the
    // memory/control dependence resolved.
    if (inst->isStoreInst()) {
      inst->writeback(cpu);
    }
    ++issued;
    this->inflyInstStatus.at(id) = InstStatus::ISSUED;
    readyIter = this->readyInsts.erase(readyIter);
  }

  this->numIssuedDist.sample(issued);
}

void AbstractDataFlowCore::commit() {
  for (auto id : this->rob) {
    auto inst = this->inflyInstMap.at(id);
    inst->tick();
    if (this->inflyInstStatus.at(id) == InstStatus::ISSUED) {
      bool done = inst->isCompleted();
      if (inst->isStoreInst()) {
        done &= inst->isWritebacked();
      }
      if (done) {
        this->inflyInstStatus.at(id) = InstStatus::FINISHED;
      }
    }
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
    ++committed;
    iter = this->rob.erase(iter);
    auto inst = this->inflyInstMap.at(id);
    this->inflyInstStatus.erase(id);
    this->inflyInstAge.erase(id);
    this->inflyInstMap.erase(id);
    this->dataFlow->commit(inst);
  }
  this->numCommittedInst += committed;
  this->numCommittedDist.sample(committed);
}

AbstractDataFlowAccelerator::AbstractDataFlowAccelerator()
    : TDGAccelerator(), handling(NONE), dataFlow(nullptr) {}
AbstractDataFlowAccelerator::~AbstractDataFlowAccelerator() {
  if (this->dataFlow != nullptr) {
    delete this->dataFlow;
    this->dataFlow = nullptr;
  }
}

void AbstractDataFlowAccelerator::handshake(LLVMTraceCPU *_cpu,
                                            TDGAcceleratorManager *_manager) {
  TDGAccelerator::handshake(_cpu, _manager);

  auto cpuParams = dynamic_cast<const LLVMTraceCPUParams *>(_cpu->params());
  this->numCores = cpuParams->adfaNumCores;
  this->enableTLS = cpuParams->adfaEnableTLS;

  for (int i = 0; i < this->numCores; ++i) {
    auto id = this->manager->name() + ".adfa.core" + std::to_string(i);
    this->cores.emplace_back(id, _cpu);
  }
}

void AbstractDataFlowAccelerator::regStats() {
  DPRINTF(AbstractDataFlowAccelerator, "ADFA: CALLED REGSTATS\n");
  this->numConfigured.name(this->manager->name() + ".adfa.numConfigured")
      .desc("Number of times ADFA get configured")
      .prereq(this->numConfigured);
  this->numExecution.name(this->manager->name() + ".adfa.numExecution")
      .desc("Number of times ADFA get executed")
      .prereq(this->numExecution);
  this->numCycles.name(this->manager->name() + ".adfa.numCycles")
      .desc("Number of cycles ADFA is running")
      .prereq(this->numCycles);
  this->numCommittedInst.name(this->manager->name() + ".adfa.numCommittedInst")
      .desc("Number of insts ADFA committed")
      .prereq(this->numCommittedInst);

  for (auto &core : this->cores) {
    core.regStats();
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
    // Simply open the data flow stream.
    if (this->dataFlow == nullptr) {
      this->dataFlow = new DynamicInstructionStream(
          inst->getTDG().adfa_config().data_flow());
    }
    this->numConfigured++;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start configure.\n");
    return true;
  } else if (auto StartInst = dynamic_cast<ADFAStartInst *>(inst)) {
    this->handling = START;
    this->currentInst.start = StartInst;
    this->numExecution++;
    this->cores[0].start(this->dataFlow);
    return true;
  }
  return false;
}

void AbstractDataFlowAccelerator::dump() {
  for (auto &core : this->cores) {
    core.dump();
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
      break;
    }
    case START: {
      this->numCycles++;
      this->tickStart();
      break;
    }
    default: { panic("Unknown handling instruction."); }
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
  bool busy = false;
  for (auto &core : this->cores) {
    core.tick();
    busy = busy || core.isBusy();
  }

  if (!busy) {
    // Simply mark the instruction finished for now.
    this->currentInst.start->markFinished();
    this->currentInst.start = nullptr;
    this->handling = NONE;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start execution: DONE.\n");
  }
}
