#include "adfa.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "cpu/llvm_trace/dyn_inst_stream.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/AbstractDataFlowAccelerator.hh"

AbstractDataFlowAccelerator::AbstractDataFlowAccelerator()
    : TDGAccelerator(), issueWidth(16), robSize(512), dataFlow(nullptr) {}
AbstractDataFlowAccelerator::~AbstractDataFlowAccelerator() {
  if (this->dataFlow != nullptr) {
    delete this->dataFlow;
    this->dataFlow = nullptr;
  }
}

void AbstractDataFlowAccelerator::regStats() {
  DPRINTF(AbstractDataFlowAccelerator, "ADFA: CALLED REGSTATS\n");
  this->numIssuedDist.init(0, this->issueWidth, 1)
      .name(this->manager->name() + ".adfa.issued_per_cycle")
      .desc("Number of inst issued each cycle")
      .flags(Stats::pdf);
  this->numCommittedDist.init(0, 8, 1)
      .name(this->manager->name() + ".adfa.committed_per_cycle")
      .desc("Number of insts committed each cycle")
      .flags(Stats::pdf);
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
    this->endToken = nullptr;
    this->currentAge = 0;
    this->inflyInstAge.clear();
    this->inflyInstStatus.clear();
    this->inflyInstMap.clear();
    this->rob.clear();
    this->readyInsts.clear();
    this->numExecution++;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start execution.\n");
    return true;
  }
  return false;
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

  this->fetch();
  this->markReady();
  this->issue();
  this->commit();
  this->release();

  if (this->endToken != nullptr && this->rob.empty()) {

    this->dataFlow->commit(this->endToken);
    this->endToken = nullptr;

    // Simply mark the instruction finished for now.
    this->currentInst.start->markFinished();
    this->currentInst.start = nullptr;
    this->handling = NONE;
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: start execution: DONE.\n");
  }
}

void AbstractDataFlowAccelerator::fetch() {

  if (this->endToken != nullptr) {
    return;
  }

  // We maintain a crazy huge rob size.
  if (this->rob.size() >= this->robSize) {
    return;
  }

  // Let's parse more instructions if the number of parsed is below a threshold.
  if (this->dataFlow->size() < 10000) {
    this->dataFlow->parse();
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

void AbstractDataFlowAccelerator::markReady() {
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
    for (auto depId : inst->getTDG().deps()) {
      auto statusIter = this->inflyInstStatus.find(depId);
      if (statusIter != this->inflyInstStatus.end() &&
          statusIter->second != InstStatus::FINISHED) {
        // The dependent instruction has not finished.
        ready = false;
        break;
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

void AbstractDataFlowAccelerator::issue() {
  size_t issued = 0;
  for (auto id : this->readyInsts) {
    // Some issue width.
    if (issued > this->issueWidth) {
      break;
    }
    DPRINTF(AbstractDataFlowAccelerator, "ADFA: issue inst %u.\n", id);
    auto inst = this->inflyInstMap.at(id);
    inst->execute(cpu);
    // For store instruction, we write back immediately as we have all the
    // memory/control dependence resolved.
    if (inst->isStoreInst()) {
      inst->writeback(cpu);
    }
    ++issued;
    this->inflyInstStatus.at(id) = InstStatus::ISSUED;
  }
  for (size_t i = 0; i < issued; ++i) {
    this->readyInsts.pop_front();
  }
  this->numIssuedDist.sample(issued);
}

void AbstractDataFlowAccelerator::commit() {
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

void AbstractDataFlowAccelerator::release() {
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