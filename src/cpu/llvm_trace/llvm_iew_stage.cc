
#include "cpu/llvm_trace/llvm_iew_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMIEWStage::LLVMIEWStage(LLVMTraceCPUParams *params, LLVMTraceCPU *_cpu)
    : cpu(_cpu),
      dispatchWidth(params->dispatchWidth),
      issueWidth(params->issueWidth),
      writeBackWidth(params->writeBackWidth),
      robSize(params->robSize),
      cacheLoadPorts(params->cacheLoadPorts),
      instQueueSize(params->instQueueSize),
      loadQueueSize(params->loadQueueSize),
      storeQueueSize(params->storeQueueSize),
      fromRenameDelay(params->renameToIEWDelay),
      toCommitDelay(params->iewToCommitDelay),
      lsq(nullptr) {
  this->lsq =
      new TDGLoadStoreQueue(this->cpu, this, this->loadQueueSize,
                            this->storeQueueSize, params->cacheStorePorts);
}

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

std::string LLVMIEWStage::name() { return cpu->name() + ".iew"; }

void LLVMIEWStage::regStats() {
  this->statIssuedInstType.init(cpu->numThreads, Enums::Num_OpClass)
      .name(name() + ".FU_type")
      .desc("Type of FU issued")
      .flags(Stats::total | Stats::pdf | Stats::dist);
  this->statIssuedInstType.ysubnames(Enums::OpClassStrings);

#define scalar(stat, describe) \
  this->stat.name(name() + ("." #stat)).desc(describe).prereq(this->stat)
  scalar(blockedCycles, "Number of cycles blocked");
  scalar(robReads, "Number of rob reads");
  scalar(robWrites, "Number of rob writes");
  scalar(intInstQueueReads, "Number of int inst queue reads");
  scalar(intInstQueueWrites, "Number of int inst queue writes");
  scalar(intInstQueueWakeups, "Number of int inst queue wakeups");
  scalar(fpInstQueueReads, "Number of fp inst queue reads");
  scalar(fpInstQueueWrites, "Number of fp inst queue writes");
  scalar(fpInstQueueWakeups, "Number of fp inst queue wakeups");

  scalar(intRegReads, "Number of int regfile reads");
  scalar(intRegWrites, "Number of int regfile writes");
  scalar(fpRegReads, "Number of fp regfile reads");
  scalar(fpRegWrites, "Number of fp regfile writes");

  scalar(ALUAccesses, "Number of ALU used");
  scalar(MultAccesses, "Number of multiplier used");
  scalar(FPUAccesses, "Number of FPU used");

  scalar(ALUAccessesCycles, "Total used cycles of ALU");
  scalar(MultAccessesCycles, "Total used cycles of multiplier");
  scalar(FPUAccessesCycles, "Total used cycles of FPU");

  scalar(execLoadInsts, "Number of executed load insts");
  scalar(execStoreInsts, "Number of executed store insts");

#undef scalar

  this->numIssuedDist.init(0, this->issueWidth, 1)
      .name(name() + ".issued_per_cycle")
      .desc("Number of insts issued each cycle")
      .flags(Stats::pdf);
  this->numExecutingDist.init(0, 192, 8)
      .name(name() + ".executing_per_cycle")
      .desc("Number of insts executing each cycle")
      .flags(Stats::pdf);
}

void LLVMIEWStage::dumpROB() const {
  inform("ROB ======================================\n");
  for (const auto &instId : this->rob) {
    const auto inst = cpu->getInflyInst(instId);
    inst->dumpDeps(cpu);
  }
  inform("ROB End ==================================\n");
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
      if (inst->isCompleted() /*&& inst->canWriteBack(cpu)*/) {
        DPRINTF(LLVMTraceCPU,
                "Inst %u finished by fu, write back, remaining %d\n", instId,
                queue.size());

        cpu->inflyInstStatus.at(instId) = InstStatus::FINISHED;

        // // If this is not store, we can immediately say this
        // // inst is write backed.
        // if (!inst->isStoreInst()) {
        //   cpu->inflyInstStatus.at(instId) = InstStatus::WRITEBACKED;
        // }

        writebacked++;
        if (inst->isFloatInst()) {
          this->fpInstQueueWakeups++;
          this->fpRegWrites += inst->getNumResults();
        } else {
          this->intInstQueueWakeups++;
          this->intRegWrites += inst->getNumResults();
        }
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
    this->robWrites++;
  }

  // Free FUs.
  cpu->fuPool->processFreeUnits();

  // Keep writing back stores.
  this->lsq->writebackStore();

  if (this->signal->stall) {
    this->blockedCycles++;
  }

  if (!this->signal->stall) {
    this->dispatch();

    unsigned writebacked = 0;
    this->writeback(this->rob, writebacked);

    this->sendToCommit();

    // Mark ready inst for next cycle.
    this->markReady();

    // Issue inst to execute.
    this->issue();
  }

  this->numExecutingDist.sample(this->rob.size());

  // Raise the stall if instQueue is too large.
  this->signal->stall = (this->rob.size() >= this->robSize ||
                         this->instQueue.size() >= this->instQueueSize ||
                         this->lsq->stores() >= this->storeQueueSize ||
                         this->lsq->loads() >= this->loadQueueSize);
}

void LLVMIEWStage::issue() {
  unsigned issuedInsts = 0;
  unsigned usedCacheLoadPorts = 0;

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

      if (inst->isLoadInst() && usedCacheLoadPorts == this->cacheLoadPorts) {
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
        /**
         * Update statisitcs.
         */
        if (inst->isFloatInst()) {
          this->fpInstQueueReads++;
          this->fpRegReads += inst->getNumOperands();
        } else {
          this->intInstQueueReads++;
          this->intRegReads += inst->getNumOperands();
        }
        if (inst->getInstName() == "load") {
          this->execLoadInsts++;
        } else if (inst->getInstName() == "store") {
          this->execStoreInsts++;
        }
        if (opClass != No_OpClass) {
          auto opLatency = cpu->getOpLatency(opClass);
          switch (opClass) {
            case IntAluOp: {
              this->ALUAccesses++;
              this->ALUAccessesCycles += opLatency;
              break;
            }
            case IntMultOp:
            case IntDivOp: {
              this->MultAccesses++;
              this->MultAccessesCycles += opLatency;
              break;
            }
            case FloatAddOp:
            case FloatMultOp:
            case FloatDivOp:
            case FloatCvtOp:
            case FloatCmpOp: {
              this->FPUAccesses++;
              this->FPUAccessesCycles += opLatency;
              break;
            }
            default: { break; }
          }
        }

        DPRINTF(LLVMTraceCPU, "Inst %u %s issued\n", instId, instName.c_str());
        // Special case for load/store.
        if (inst->isLoadInst()) {
          this->lsq->executeLoad(instId);
          usedCacheLoadPorts++;
        } else if (inst->isStoreInst()) {
          this->lsq->executeStore(instId);
        } else {
          inst->execute(cpu);
        }
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

    // Store queue full.
    if (inst->isStoreInst() && this->lsq->stores() == this->storeQueueSize) {
      break;
    }

    // Load queue full.
    if (inst->isLoadInst() && this->lsq->loads() == this->loadQueueSize) {
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
    if (inst->isFloatInst()) {
      this->fpInstQueueWrites++;
    } else {
      this->intInstQueueWrites++;
    }

    this->instQueue.push_back(instId);
    if (inst->isStoreInst()) {
      this->lsq->insertStore(instId);
    }
    if (inst->isLoadInst()) {
      this->lsq->insertLoad(instId);
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

void LLVMIEWStage::blockMemInst(LLVMDynamicInstId instId) {
  // hack("Block memory instructions %lu.\n", instId);
  auto statusIter = cpu->inflyInstStatus.find(instId);
  panic_if(statusIter == cpu->inflyInstStatus.end(),
           "Inst %u should be in inflyInstStatus to be blocked.", instId);
  panic_if(statusIter->second != InstStatus::ISSUED,
           "Inst should be in issued state to be blocked.");
  statusIter->second = InstStatus::BLOCKED;
}

void LLVMIEWStage::unblockMemoryInsts() {
  // hack("Unblock memory instructions.\n");
  for (auto instId : this->instQueue) {
    auto statusIter = cpu->inflyInstStatus.find(instId);
    panic_if(statusIter == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to be unblocked.", instId);
    if (statusIter->second != InstStatus::BLOCKED) {
      continue;
    }
    // hack("Unblock memory instructions %lu.\n", instId);
    statusIter->second = InstStatus::READY;
  }
}

void LLVMIEWStage::commitInst(LLVMDynamicInstId instId) {
  panic_if(this->rob.empty(), "ROB empty when commitInst for %lu.", instId);
  panic_if(this->rob.front() != instId, "Unmatchted rob header inst.");
  panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
           "Inst %u should be in inflyInstStatus to check if writebacked.\n",
           instId);

  auto &instStatus = cpu->inflyInstStatus.at(instId);
  panic_if(instStatus != InstStatus::COMMIT, "Inst is not in COMMIT status.");

  auto inst = cpu->inflyInstMap.at(instId);
  if (inst->isStoreInst()) {
    instStatus = InstStatus::COMMITTING;
    this->lsq->commitStore(instId);
  } else {
    // For other cases, the instruction is automatically committed.
    instStatus = InstStatus::COMMITTED;
  }
}

void LLVMIEWStage::postCommitInst(LLVMDynamicInstId instId) {
  panic_if(this->rob.empty(), "ROB empty when commitInst for %lu.", instId);
  panic_if(this->rob.front() != instId, "Unmatchted rob header inst.");
  panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
           "Inst %u should be in inflyInstStatus to check if committed.\n",
           instId);

  auto &instStatus = cpu->inflyInstStatus.at(instId);
  panic_if(instStatus != InstStatus::COMMITTED,
           "Inst is not in committed status.");
  this->rob.pop_front();

  // Memory instruction can not be removed from the instruction queue
  // until committed.
  auto inst = cpu->inflyInstMap.at(instId);
  if (inst->isLoadInst() || inst->isStoreInst()) {
    auto instQueueIter =
        std::find(this->instQueue.begin(), this->instQueue.end(), instId);
    if (instQueueIter == this->instQueue.end()) {
      panic("Failed to find mem inst %u in inst queue.\n", instId);
    }
    this->instQueue.erase(instQueueIter);
  }

  if (inst->isLoadInst()) {
    this->lsq->postCommitLoad(instId);
  }
}

void LLVMIEWStage::sendToCommit() {
  // Commit must be in order.
  unsigned committedInst = 0;
  auto commitIter = this->rob.begin();
  while (committedInst < 8 && commitIter != this->rob.end()) {
    auto instId = *commitIter;
    /**
     * Simply checking if the header of the rob is ready to commit is one read.
     */
    this->robReads++;
    panic_if(cpu->inflyInstStatus.find(instId) == cpu->inflyInstStatus.end(),
             "Inst %u should be in inflyInstStatus to check if writebacked.\n",
             instId);

    bool canCommit = false;
    auto &instStatus = cpu->inflyInstStatus.at(instId);
    canCommit = (instStatus == InstStatus::FINISHED);

    if (canCommit) {
      instStatus = InstStatus::COMMIT;
      this->toCommit->push_back(instId);

      DPRINTF(LLVMTraceCPU, "Inst %u %s is sent to commit.\n", instId,
              cpu->inflyInstMap.at(instId)->getInstName().c_str());
      committedInst++;

      commitIter++;

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
  auto instStatus = cpu->inflyInstStatus.at(instId);
  if (instStatus == InstStatus::BLOCKED) {
    // Blocked instruction is ignored.
    return;
  }
  if (instStatus != InstStatus::ISSUED) {
    panic("processFUCompletion: Inst %u is not issued\n", instId);
  }
}

LLVMIEWStage::FUCompletion::FUCompletion(LLVMDynamicInstId _instId, int _fuId,
                                         LLVMIEWStage *_iew, bool _shouldFreeFU)
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

const char *LLVMIEWStage::FUCompletion::description() const {
  return "Function unit completion";
}
