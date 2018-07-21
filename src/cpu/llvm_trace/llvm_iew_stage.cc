
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMIEWStage::LLVMIEWStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu)
    : cpu(_cpu), dispatchWidth(params->dispatchWidth),
      issueWidth(params->issueWidth), writeBackWidth(params->writeBackWidth),
      robSize(params->robSize), instQueueSize(params->instQueueSize),
      loadQueueSize(params->loadQueueSize),
      storeQueueSize(params->storeQueueSize),
      fromRenameDelay(params->renameToIEWDelay),
      toCommitDelay(params->iewToCommitDelay), loadQueueN(0) {}

void LLVMIEWStage::setFromRename(TimeBuffer<RenameStruct> *fromRenameBuffer) {
  this->fromRename = fromRenameBuffer->getWire(-this->fromRenameDelay);
}

void LLVMIEWStage::setToCommit(TimeBuffer<IEWStruct> *toCommitBuffer) {
  this->toCommit = toCommitBuffer->getWire(0);
}

void LLVMIEWStage::setSignal(TimeBuffer<LLVMStageSignal> *signalBuffer,
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

void LLVMIEWStage::writeback(std::list<LLVMDynamicInstId> &queue,
                             unsigned &writebacked) {
  // Send finished inst to commit stage if not stalled.
  auto iter = queue.begin();
  while (iter != queue.end() && writebacked < this->writeBackWidth) {
    auto instId = *iter;
    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to check if completed.\n",
             instId);

    // DPRINTF(LLVMTraceCPU, "Inst %u check if issued in write back\n", instId);
    if (cpu->inflyInstStatus.at(instId) == InstStatus::ISSUED) {
      // Tick the inst.

      // DPRINTF(LLVMTraceCPU, "Inst %u is labelled issued in write back\n",
      //         instId);
      auto inst = cpu->inflyInstMap.at(instId);
      inst->tick();

      // Check if the inst is finished by FU.
      if (inst->isCompleted() && inst->canWriteBack(cpu)) {
        DPRINTF(LLVMTraceCPU,
                "Inst %u finished by fu, write back, remaining %d\n", instId,
                queue.size());

        cpu->inflyInstStatus.at(instId) = InstStatus::FINISHED;

        // If this is not store, we can immediately say this
        // inst is write backed.
        if (!inst->isStoreInst()) {
          cpu->inflyInstStatus.at(instId) = InstStatus::WRITEBACKED;
        }

        writebacked++;
        continue;
      }
    }

    // Otherwise, increase the iter.
    ++iter;
  }
}

void LLVMIEWStage::writebackStoreQueue() {
  // Check the head of the store queue.
  auto iter = this->storeQueue.begin();
  while (iter != this->storeQueue.end()) {
    auto instId = *iter;
    auto inst = cpu->inflyInstMap.at(instId);

    panic_if(!inst->isStoreInst(), "Non-store inst %u in store queue.\n",
             instId);
    // Check if write backed.
    if (cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end()) {
      panic("Inst %u should be in inflyInstStatus to check status.\n", instId);
    }
    auto status = cpu->inflyInstStatus.at(instId);
    if (status == InstStatus::WRITEBACKING) {
      // This inst is already been writing back.
      // Check if it's done.
      if (inst->isWritebacked()) {
        // If so, update it to writebacked and remove from the store queue.
        // Continue to try to write back the next store in queue.
        DPRINTF(LLVMTraceCPU, "Store inst %u is writebacked.\n", instId);
        cpu->inflyInstStatus.at(instId) = InstStatus::WRITEBACKED;
        iter = this->storeQueue.erase(iter);
        // Uppon erase from the store queue, we must also remove from
        // the inst queue.
        // TODO: optimize this.
        auto instQueueIter =
            std::find(this->instQueue.begin(), this->instQueue.end(), instId);
        if (instQueueIter == this->instQueue.end()) {
          panic("Failed to find store inst %u in inst queue.\n", instId);
        }
        this->instQueue.erase(instQueueIter);
        continue;
      } else {
        // The head of the store queue is not done.
        // break.
        break;
      }
    } else if (status == InstStatus::FINISHED) {
      // The inst is still waiting to be written back.
      // This cycle is done.
      DPRINTF(LLVMTraceCPU, "Store inst %u is started to writeback.\n", instId);
      cpu->inflyInstStatus.at(instId) = InstStatus::WRITEBACKING;
      inst->writeback(cpu);
      break;
    } else {
      // This inst is not issued/finished, break.
      break;
    }
  }
}

void LLVMIEWStage::tick() {
  // Get inst from rename.
  for (auto iter = this->fromRename->begin(), end = this->fromRename->end();
       iter != end; ++iter) {
    this->rob.push_back(*iter);
  }

  // Free FUs.
  cpu->fuPool->processFreeUnits();

  if (this->signal->stall) {
    this->blockedCycles++;
  }

  if (!this->signal->stall) {
    this->dispatch();

    unsigned writebacked = 0;
    this->writeback(this->rob, writebacked);
    this->writebackStoreQueue();

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
                         this->storeQueue.size() >= this->storeQueueSize ||
                         this->loadQueueN >= this->loadQueueSize);
}

void LLVMIEWStage::issue() {
  unsigned issuedInsts = 0;

  auto iter = this->instQueue.begin();
  while (iter != this->instQueue.end() && issuedInsts < this->issueWidth) {
    auto instId = *iter;
    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to check if READY.\n",
             instId);

    bool shouldRemoveFromInstQueue = false;

    if (cpu->inflyInstStatus.at(instId) == InstStatus::READY) {
      bool canIssue = true;
      auto inst = cpu->inflyInstMap.at(instId);
      auto opClass = inst->getOpClass();
      auto fuId = FUPool::NoCapableFU;
      const auto &instName = inst->getInstName();

      // Check if there is enough issueWidth.
      if (issuedInsts + inst->getQueueWeight() > this->issueWidth) {
        continue;
      }

      // Check if there is available FU.
      if (opClass != No_OpClass) {
        fuId = cpu->fuPool->getUnit(opClass);
        panic_if(fuId == FUPool::NoCapableFU,
                 "There is no capable FU %s for inst %u.\n",
                 Enums::OpClassStrings[opClass], instId);
        if (fuId == FUPool::NoFreeFU) {
          canIssue = false;
        }
      }

      if (canIssue) {
        cpu->inflyInstStatus.at(instId) = InstStatus::ISSUED;
        issuedInsts += cpu->inflyInstMap.at(instId)->getQueueWeight();
        DPRINTF(LLVMTraceCPU, "Inst %u %s issued\n", instId, instName.c_str());
        inst->execute(cpu);
        if (!cpu->isStandalone()) {
          this->statIssuedInstType[cpu->thread_context->threadId()][opClass]++;
        } else {
          this->statIssuedInstType[0][opClass]++;
        }
        // After issue, if the inst is not load/store, we can remove them
        // from the inst queue.
        if (!inst->isStoreInst() && !inst->isLoadInst()) {
          shouldRemoveFromInstQueue = true;
        }

        // Handle the FU completion.
        if (opClass != No_OpClass) {
          // DPRINTF(LLVMTraceCPU, "Inst %u get FU %s with fuId %d.\n", instId,
          //         Enums::OpClassStrings[opClass], fuId);

          auto opLatency = cpu->getOpLatency(opClass);
          // For accelerator, use the latency from the "actual accelerator".
          if (opClass == Enums::OpClass::Accelerator) {
            panic("Accelerator is deprecated now.");
          }

          if (opLatency == Cycles(1)) {
            this->processFUCompletion(instId, fuId);
          } else {
            bool pipelined = cpu->fuPool->isPipelined(opClass);
            FUCompletion *fuCompletion =
                new FUCompletion(instId, fuId, this, !pipelined);
            // Schedule the event.
            // Notice for the -1 part so that we can free FU in next cycle.
            cpu->schedule(fuCompletion, cpu->clockEdge(Cycles(opLatency - 1)));
            // If pipelined, mark the FU free immediately for next cycle.
            if (pipelined) {
              cpu->fuPool->freeUnitNextCycle(fuId);
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
  /**
   * Dispatch is always in order.
   */
  for (auto iter = this->rob.begin(), end = this->rob.end();
       iter != end && dispatchedInst < this->dispatchWidth; ++iter) {
    if (this->instQueue.size() == this->instQueueSize) {
      break;
    }

    auto instId = *iter;
    auto inst = cpu->inflyInstMap.at(instId);

    if (inst->isStoreInst() &&
        this->storeQueue.size() == this->storeQueueSize) {
      break;
    }

    if (inst->isLoadInst() && this->loadQueueN == this->loadQueueSize) {
      break;
    }

    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to check if DECODED\n",
             instId);

    if (cpu->inflyInstStatus.at(instId) != InstStatus::DECODED) {
      continue;
    }

    /**
     * We can actually dispatch this instruction now, but we
     * have to handle serialization instruction.
     * A SerializeAfter instruction will mark the next instruction
     * SerializeBefore.
     *
     * A SerializeBefore instruction will not be dispatched until it reaches the
     * head of rob.
     */
    if (inst->isSerializeBefore()) {
      if (iter != this->rob.begin()) {
        // Do not dispatch until we reached the head of rob for SerializeBefore
        // instruction.
        break;
      }
    }

    // Before we dispatch, we update the region stats.
    const auto &TDG = inst->getTDG();
    if (TDG.bb() != 0) {
      cpu->updateBasicBlock(TDG.bb());
    }

    cpu->inflyInstStatus.at(instId) = InstStatus::DISPATCHED;
    dispatchedInst++;
    this->instQueue.push_back(instId);
    if (inst->isStoreInst()) {
      this->storeQueue.push_back(instId);
    }
    if (inst->isLoadInst()) {
      this->loadQueueN++;
    }
    DPRINTF(LLVMTraceCPU, "Inst %u is dispatched to instruction queue.\n",
            instId);
  }
}

void LLVMIEWStage::markReady() {
  for (auto iter = this->instQueue.begin(), end = this->instQueue.end();
       iter != end; ++iter) {
    auto instId = *iter;
    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to check if READY\n",
             instId);
    if (cpu->inflyInstStatus.at(instId) == InstStatus::DISPATCHED) {
      auto inst = cpu->inflyInstMap.at(instId);
      if (inst->isDependenceReady(cpu)) {
        // Mark the status to ready.
        DPRINTF(LLVMTraceCPU, "Inst %u is marked ready in instruction queue.\n",
                instId);
        cpu->inflyInstStatus.at(instId) = InstStatus::READY;
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
    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to check if writebacked.\n",
             instId);
    if (cpu->inflyInstStatus.at(instId) == InstStatus::WRITEBACKED) {
      this->rob.erase(head);
      this->toCommit->push_back(instId);

      auto inst = cpu->inflyInstMap.at(instId);

      // Special case for load, it can be removed from load queue
      // once committed.
      if (inst->isLoadInst()) {
        auto instQueueIter =
            std::find(this->instQueue.begin(), this->instQueue.end(), instId);
        if (instQueueIter == this->instQueue.end()) {
          panic("Failed to find load inst %u in inst queue.\n", instId);
        }
        this->instQueue.erase(instQueueIter);
        this->loadQueueN--;
      }

      DPRINTF(LLVMTraceCPU, "Inst %u %s is sent to commit.\n", instId,
              inst->getInstName().c_str());
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
    cpu->fuPool->freeUnitNextCycle(fuId);
  }
  // Check that this inst is legal and already be issued.
  if (cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end()) {
    panic(
        "processFUCompletion: Failed to find the inst %u in inflyInstStatus\n",
        instId);
  }
  if (cpu->inflyInstStatus.at(instId) != InstStatus::ISSUED) {
    panic("processFUCompletion: Inst %u is not issued\n", instId);
  }
}

LLVMIEWStage::FUCompletion::FUCompletion(LLVMDynamicInstId _instId, int _fuId,
                                         LLVMIEWStage *_iew, bool _shouldFreeFU)
    : Event(Stat_Event_Pri, AutoDelete), instId(_instId), fuId(_fuId),
      iew(_iew), shouldFreeFU(_shouldFreeFU) {}

void LLVMIEWStage::FUCompletion::process() {
  // Call the process function from cpu.
  this->iew->processFUCompletion(this->instId,
                                 this->shouldFreeFU ? this->fuId : -1);
}

const char *LLVMIEWStage::FUCompletion::description() const {
  return "Function unit completion";
}
