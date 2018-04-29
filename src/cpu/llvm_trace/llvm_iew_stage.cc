
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_accelerator.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMIEWStage::LLVMIEWStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      dispatchWidth(params->dispatchWidth),
      issueWidth(params->issueWidth),
      writeBackWidth(params->writeBackWidth),
      robSize(params->robSize),
      instQueueSize(params->instQueueSize),
      loadQueueSize(params->loadQueueSize),
      storeQueueSize(params->storeQueueSize),
      fromRenameDelay(params->renameToIEWDelay),
      toCommitDelay(params->iewToCommitDelay),
      loadQueueN(0),
      storeQueueN(0),
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

  this->blockedCycles.name(cpu->name() + ".iew.blockedCycles")
      .desc("Number of cycles blocked")
      .prereq(this->blockedCycles);

  this->numIssuedDist.init(0, this->issueWidth, 1)
      .name(cpu->name() + ".iew.issued_per_cycle")
      .desc("Number of insts issued each cycle")
      .flags(Stats::pdf);
  this->numExecutingDist.init(0, 192, 8)
      .name(cpu->name() + ".iew.executing_per_cycle")
      .desc("Number of insts executing each cycle")
      .flags(Stats::pdf);
}

void LLVMIEWStage::writeback(std::list<LLVMDynamicInstId>& queue,
                             unsigned& writebacked) {
  // Send finished inst to commit stage if not stalled.
  auto iter = queue.begin();
  while (iter != queue.end() && writebacked < this->writeBackWidth) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if completed.\n",
             instId);

    // DPRINTF(LLVMTraceCPU, "Inst %u check if issued in write back\n", instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::ISSUED) {
      // Tick the inst.

      // DPRINTF(LLVMTraceCPU, "Inst %u is labelled issued in write back\n",
      //         instId);
      auto inst = cpu->dynamicInsts[instId];
      inst->tick();

      // Check if the inst is finished by FU.
      if (inst->isCompleted() && inst->canWriteBack(cpu)) {
        DPRINTF(LLVMTraceCPU, "Inst %u finished by fu, write back, remaining %d\n",
                instId, queue.size());

        inst->writeback(cpu);

        cpu->inflyInsts[instId] = InstStatus::FINISHED;

        // Special case for store, it can be removed from store queue
        // once write backed.
        // TODO: optimize this.
        if (inst->getInstName() == "store") {
          bool found = false;
          for (auto instQueueIter = this->instQueue.begin(),
                    instQueueEnd = this->instQueue.end();
               instQueueIter != instQueueEnd; ++instQueueIter) {
            if ((*instQueueIter) == instId) {
              this->instQueue.erase(instQueueIter);
              this->storeQueueN--;
              found = true;
              break;
            }
          }
          // Panic if not found.
          panic_if(!found, "Failed to find store inst %u in the inst queue.\n",
                   instId);
        }

        writebacked++;
        continue;
      }
    }

    // Otherwise, increase the iter.
    ++iter;
  }
}

void LLVMIEWStage::tick() {
  // Get inst from rename.
  for (auto iter = this->fromRename->begin(), end = this->fromRename->end();
       iter != end; ++iter) {
    this->rob.push_back(*iter);
  }

  // Free FUs.
  this->fuPool->processFreeUnits();

  if (this->signal->stall) {
    this->blockedCycles++;
  }

  if (!this->signal->stall) {
    this->dispatch();

    unsigned writebacked = 0;
    this->writeback(this->rob, writebacked);

    this->commit();

    // Mark ready inst for next cycle.
    this->markReady();

    // Issue inst to execute.
    this->issue();
  }

  this->numExecutingDist.sample(this->rob.size());

  // Raise the stall if instQueue is too large.
  this->signal->stall = (this->rob.size() >= this->robSize ||
                         this->instQueue.size() >= this->instQueueSize ||
                         this->storeQueueN >= this->storeQueueSize ||
                         this->loadQueueN >= this->loadQueueSize);
}

void LLVMIEWStage::issue() {
  unsigned issuedInsts = 0;

  auto iter = this->instQueue.begin();
  while (iter != this->instQueue.end() && issuedInsts < this->issueWidth) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if READY.\n", instId);

    bool shouldRemoveFromInstQueue = false;

    if (cpu->inflyInsts.at(instId) == InstStatus::READY) {
      bool canIssue = true;
      auto inst = cpu->dynamicInsts[instId];
      auto opClass = inst->getOpClass();
      auto fuId = FUPool::NoCapableFU;
      const auto& instName = inst->getInstName();

      // Check if there is enough issueWidth.
      if (issuedInsts + inst->getQueueWeight() > this->issueWidth) {
        continue;
      }

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
        issuedInsts += cpu->dynamicInsts[instId]->getQueueWeight();
        DPRINTF(LLVMTraceCPU, "Inst %u %s issued\n", instId, instName.c_str());
        inst->execute(cpu);
        this->statIssuedInstType[cpu->thread_context->threadId()][opClass]++;
        // After issue, if the inst is not load/store, we can remove them
        // from the inst queue.
        if (instName != "store" && instName != "load") {
          shouldRemoveFromInstQueue = true;
        }

        // Handle the FU completion.
        if (opClass != No_OpClass) {
          // DPRINTF(LLVMTraceCPU, "Inst %u get FU %s with fuId %d.\n", instId,
          //         Enums::OpClassStrings[opClass], fuId);
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
            if (pipelined) {
              this->fuPool->freeUnitNextCycle(fuId);
            }
          }
        }
      }
    }
    if (shouldRemoveFromInstQueue) {
      iter = this->instQueue.erase(iter);
    } else {
      ++iter;
    }
  }

  this->numIssuedDist.sample(issuedInsts);
}

void LLVMIEWStage::dispatch() {
  unsigned dispatchedInst = 0;
  for (auto iter = this->rob.begin(), end = this->rob.end();
       iter != end && dispatchedInst < this->dispatchWidth; ++iter) {
    if (this->instQueue.size() == this->instQueueSize) {
      break;
    }

    auto instId = *iter;
    const auto& instName = cpu->dynamicInsts[instId]->getInstName();
    if (instName == "store" && this->storeQueueN == this->storeQueueSize) {
      break;
    }

    if (instName == "load" && this->loadQueueN == this->loadQueueSize) {
      break;
    }

    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if READY\n", instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::DECODED) {
      cpu->inflyInsts[instId] = InstStatus::DISPATCHED;
      dispatchedInst++;
      this->instQueue.push_back(instId);
      if (instName == "store") {
        this->storeQueueN++;
      }
      if (instName == "load") {
        this->loadQueueN++;
      }
      DPRINTF(LLVMTraceCPU, "Inst %u is dispatched to instruction queue.\n",
              instId);
    }
  }
}

void LLVMIEWStage::markReady() {
  for (auto iter = this->instQueue.begin(), end = this->instQueue.end();
       iter != end; ++iter) {
    auto instId = *iter;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if READY\n", instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::DISPATCHED) {
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

void LLVMIEWStage::commit() {
  // Commit must be in order.
  unsigned committedInst = 0;
  while (committedInst < 8) {
    if (this->rob.empty()) {
      return;
    }
    auto head = this->rob.begin();
    auto instId = *head;
    panic_if(cpu->inflyInsts.find(instId) == cpu->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if FINISHED.\n", instId);
    if (cpu->inflyInsts.at(instId) == InstStatus::FINISHED) {
      this->rob.erase(head);
      this->toCommit->push_back(instId);

      // Special case for load, it can be removed from load queue
      // once committed.
      // TODO: optimize this loop away.
      if (cpu->dynamicInsts[instId]->getInstName() == "load") {
        bool found = false;
        for (auto instQueueIter = this->instQueue.begin(),
                  instQueueEnd = this->instQueue.end();
             instQueueIter != instQueueEnd; ++instQueueIter) {
          if ((*instQueueIter) == instId) {
            this->instQueue.erase(instQueueIter);
            this->loadQueueN--;
            found = true;
            break;
          }
        }
        // Panic if not found.
        panic_if(!found,
                 "Failed to find load inst %u in the inst queue when commit.\n",
                 instId);
      }

      committedInst++;
    } else {
      // The header is not finished, no need to check others.
      break;
    }
  }
}

void LLVMIEWStage::processFUCompletion(LLVMDynamicInstId instId, int fuId) {
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
