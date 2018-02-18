
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_accelerator.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMIEWStage::LLVMIEWStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      issueWidth(params->issueWidth),
      instQueueSize(params->instQueueSize),
      fromRenameDelay(params->renameToIEWDelay),
      toCommitDelay(params->iewToCommitDelay),
      fuPool(params->fuPool) {
  // The FU in FUPool is stateless, however, the accelerator may be stateful,
  // e.g. config time.
  // To handle this, FUPool is only in charge of monitoring the use/free of
  // accelerator. The actual context and latency is handled by accelerator
  // itself.
  // For every "virtual accelerator" in FUPool, construct the actual one.
  int acceleratorId = this->fuPool->getUnit(Enums::OpClass::Accelerator);
  if (acceleratorId != FUPool::NoCapableFU) {
    // We have accelerator in the system, try to get all accelerator.
    while (acceleratorId != FUPool::NoFreeFU) {
      if (this->acceleratorMap.find(acceleratorId) !=
          this->acceleratorMap.end()) {
        panic("Accelerator id %d is already occupied.\n", acceleratorId);
      }
      DPRINTF(LLVMTraceCPU, "Register accelerator %d.\n", acceleratorId);
      this->acceleratorMap[acceleratorId] = new LLVMAccelerator();
      // Add to the free list.
      this->fuPool->freeUnitNextCycle(acceleratorId);
      // Get next accelerator.
      acceleratorId = this->fuPool->getUnit(Enums::OpClass::Accelerator);
    }
    // Remember to free all the accelerators.
    this->fuPool->processFreeUnits();
  }
}

void LLVMIEWStage::setFromRename(TimeBuffer<RenameStruct>* fromRenameBuffer) {
  this->fromRename = fromRenameBuffer->getWire(-this->fromRenameDelay);
}

void LLVMIEWStage::setToCommit(TimeBuffer<IEWStruct>* toCommitBuffer) {
  this->toCommit = toCommitBuffer->getWire(0);
}

void LLVMIEWStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer,
                             int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMIEWStage::regStats() {
  this->statIssuedInstType.init(cpu->numThreads, Enums::Num_OpClass)
      .name(cpu->name() + ".iew.FU_type")
      .desc("Type of FU issued")
      .flags(Stats::total | Stats::pdf | Stats::dist);
  this->statIssuedInstType.ysubnames(Enums::OpClassStrings);
}

void LLVMIEWStage::tick() {
  // Get inst from rename.
  for (auto iter = this->fromRename->begin(), end = this->fromRename->end();
       iter != end; ++iter) {
    this->instQueue.push_back(*iter);
  }

  // Issue inst to execute.
  this->issue();

  // Send finished inst to commit stage if not stalled.
  auto iter = this->instQueue.begin();
  while (iter != this->instQueue.end()) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if completed.\n",
             instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::ISSUED) {
      // Tick the inst.
      auto inst = cpu->dynamicInsts[instId];
      inst->tick();

      // Check if the inst is finished.
      if (inst->isCompleted()) {
        // Send it to commit stage.
        DPRINTF(LLVMTraceCPU, "Inst %u finished, send to commit\n", instId);
        cpu->inflyInsts[instId] = InstStatus::FINISHED;
        if (!this->signal->stall) {
          this->toCommit->push_back(instId);
          iter = this->instQueue.erase(iter);
          continue;
        }
      }
    } else if (cpu->inflyInsts.at(instId) == InstStatus::FINISHED) {
      if (!this->signal->stall) {
        this->toCommit->push_back(instId);
        iter = this->instQueue.erase(iter);
        continue;
      }
    }

    // Otherwise, increase the iter.
    ++iter;
  }

  // Mark ready inst for next cycle.
  this->markReadyInsts();

  // Raise the stall if instQueue is too large.
  this->signal->stall = this->instQueue.size() >= this->instQueueSize;
}

void LLVMIEWStage::issue() {
  // Free FUs.
  this->fuPool->processFreeUnits();

  unsigned issuedInsts = 0;

  for (auto iter = this->instQueue.begin(), end = this->instQueue.end();
       iter != end && issuedInsts < this->issueWidth; ++iter) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if READY.\n", instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::READY) {
      bool canIssue = true;
      auto inst = cpu->dynamicInsts[instId];
      auto opClass = inst->getOpClass();
      auto fuId = FUPool::NoCapableFU;

      // Check if there is available FU.
      if (opClass != No_OpClass) {
        fuId = this->fuPool->getUnit(opClass);
        panic_if(fuId == FUPool::NoCapableFU,
                 "There is no capable FU %s for inst %u.\n",
                 Enums::OpClassStrings[opClass], instId);
        if (fuId == FUPool::NoFreeFU) {
          canIssue = false;
        }
      }

      if (canIssue) {
        cpu->inflyInsts[instId] = InstStatus::ISSUED;
        issuedInsts++;
        DPRINTF(LLVMTraceCPU, "Inst %u issued\n", instId);
        inst->execute(cpu);
        this->statIssuedInstType[cpu->thread_context->threadId()][opClass]++;

        // Handle the FU completion.
        if (opClass != No_OpClass) {
          DPRINTF(LLVMTraceCPU, "Inst %u get FU %s with fuId %d.\n", instId,
                  Enums::OpClassStrings[opClass], fuId);
          inst->startFUStatusFSM();

          auto opLatency = this->fuPool->getOpLatency(opClass);
          // For accelerator, use the latency from the "actual accelerator".
          if (opClass == Enums::OpClass::Accelerator) {
            if (this->acceleratorMap.find(fuId) == this->acceleratorMap.end()) {
              panic("Accelerator id %d has no actual accelerator.\n", fuId);
            }
            // For now hack with null context pointer.
            opLatency = this->acceleratorMap.at(fuId)->getOpLatency(
                inst->getAcceleratorContext());
          }

          if (opLatency == Cycles(1)) {
            this->processFUCompletion(instId, fuId);
          } else {
            bool pipelined = this->fuPool->isPipelined(opClass);
            FUCompletion* fuCompletion =
                new FUCompletion(instId, fuId, this, !pipelined);
            // Schedule the event.
            // Notice for the -1 part so that we can free FU in next cycle.
            cpu->schedule(fuCompletion, cpu->clockEdge(Cycles(opLatency - 1)));
            // If pipelined, mark the FU free immediately for next cycle.
            this->fuPool->freeUnitNextCycle(fuId);
          }
        }
      }
    }
  }
}

void LLVMIEWStage::markReadyInsts() {
  for (auto iter = this->instQueue.begin(), end = this->instQueue.end();
       iter != end; ++iter) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if READY\n", instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::DECODED) {
      auto inst = cpu->dynamicInsts[instId];
      if (inst->isDependenceReady(cpu)) {
        // Mark the status to ready.
        DPRINTF(LLVMTraceCPU, "Inst %u is marked ready in instruction queue.\n",
                instId);
        cpu->inflyInsts[instId] = InstStatus::READY;
      }
    }
  }
}

void LLVMIEWStage::processFUCompletion(LLVMDynamicInstId instId, int fuId) {
  DPRINTF(LLVMTraceCPU, "FUCompletion for inst %u with fu %d\n", instId, fuId);
  // Check if we should free this fu next cycle.
  if (fuId > -1) {
    this->fuPool->freeUnitNextCycle(fuId);
  }
  // Check that this inst is legal and already be issued.
  if (instId >= cpu->dynamicInsts.size()) {
    panic("processFUCompletion: Illegal inst %u\n", instId);
  }
  if (cpu->inflyInsts.find(instId) == cpu->inflyInsts.end()) {
    panic("processFUCompletion: Failed to find the inst %u in inflyInsts\n",
          instId);
  }
  if (cpu->inflyInsts.at(instId) != InstStatus::ISSUED) {
    panic("processFUCompletion: Inst %u is not issued\n", instId);
  }
  // Inform the inst that it has completed.
  cpu->dynamicInsts[instId]->handleFUCompletion();
}

LLVMIEWStage::FUCompletion::FUCompletion(LLVMDynamicInstId _instId, int _fuId,
                                         LLVMIEWStage* _iew, bool _shouldFreeFU)
    : Event(Stat_Event_Pri, AutoDelete),
      instId(_instId),
      fuId(_fuId),
      iew(_iew),
      shouldFreeFU(_shouldFreeFU) {}

void LLVMIEWStage::FUCompletion::process() {
  // Call the process function from cpu.
  this->iew->processFUCompletion(this->instId,
                                 this->shouldFreeFU ? this->fuId : -1);
}

const char* LLVMIEWStage::FUCompletion::description() const {
  return "Function unit completion";
}
