#include <fstream>

#include "cpu/thread_context.hh"
#include "debug/LLVMTraceCPU.hh"
#include "llvm_trace_cpu.hh"
#include "sim/process.hh"

LLVMTraceCPU::LLVMTraceCPU(LLVMTraceCPUParams* params)
    : BaseCPU(params),
      pageTable(params->name + ".page_table", 0),
      instPort(params->name + ".inst_port", this),
      dataPort(params->name + ".data_port", this),
      fuPool(params->fuPool),
      traceFile(params->traceFile),
      currentStackDepth(0),
      currentInstId(0),
      process(nullptr),
      thread_context(nullptr),
      stackMin(0),
      MAX_FETCH_QUEUE_SIZE(params->maxFetchQueueSize),
      MAX_REORDER_BUFFER_SIZE(params->maxReorderBufferSize),
      MAX_INSTRUCTION_QUEUE_SIZE(params->maxInstructionQueueSize),
      driver(params->driver),
      tickEvent(*this) {
  DPRINTF(LLVMTraceCPU, "LLVMTraceCPU constructed\n");
  // Initialize the rob buffer.
  this->rob = std::vector<std::pair<LLVMDynamicInstId, bool>>(
      this->MAX_REORDER_BUFFER_SIZE, std::make_pair(0, false));
  panic_if(this->MAX_REORDER_BUFFER_SIZE != this->rob.size(),
           "rob size (%d) should be MAX_REORDER_BUFFER_SIZE(%d)\n",
           this->rob.size(), this->MAX_REORDER_BUFFER_SIZE);
  readTraceFile();
  if (driver != nullptr) {
    // Handshake with the driver.
    driver->handshake(this);
  } else {
    // No driver, stand alone mode.
    // Schedule the first event.
    schedule(this->tickEvent, nextCycle());
  }
}

LLVMTraceCPU::~LLVMTraceCPU() {}

LLVMTraceCPU* LLVMTraceCPUParams::create() { return new LLVMTraceCPU(this); }

void LLVMTraceCPU::readTraceFile() {
  std::ifstream stream(this->traceFile);
  if (!stream.is_open()) {
    fatal("Failed opening trace file %s\n", this->traceFile.c_str());
  }
  for (std::string line; std::getline(stream, line);) {
    DPRINTF(LLVMTraceCPU, "read in %s\n", line.c_str());

    auto inst = parseLLVMDynamicInst(this->dynamicInsts.size(), line);
    this->dynamicInsts.push_back(inst);

    DPRINTF(LLVMTraceCPU, "Parsed #%u dynamic inst with %s\n",
            this->dynamicInsts.size(),
            this->dynamicInsts.back()->toLine().c_str());
  }
  DPRINTF(LLVMTraceCPU, "Parsed total number of inst: %u\n",
          this->dynamicInsts.size());
}

void LLVMTraceCPU::fetch() {
  // Only fetch if the stack depth is > 0.
  while (this->currentInstId < this->dynamicInsts.size() &&
         this->fetchQueue.size() < this->MAX_FETCH_QUEUE_SIZE &&
         this->currentStackDepth > 0) {
    DPRINTF(LLVMTraceCPU, "Fetch inst %d into fetchQueue\n",
            this->currentInstId);
    this->inflyInsts[currentInstId] = InstStatus::FETCHED;
    this->fetchQueue.push(this->currentInstId);
    // Update the stack depth for call/ret inst.
    this->currentStackDepth +=
        this->dynamicInsts[currentInstId]->getCallStackAdjustment();
    DPRINTF(LLVMTraceCPU, "Stack depth updated to %u\n",
            this->currentStackDepth);
    if (this->currentStackDepth < 0) {
      panic("Current stack depth is less than 0\n");
    }
    this->currentInstId++;
  }
}

void LLVMTraceCPU::reorder() {
  for (size_t i = 0; i < this->rob.size(); ++i) {
    if (!this->rob[i].second) {
      // This is a free rob entry.
      if (this->fetchQueue.empty()) {
        // No more instructions.
        return;
      }
      // Fill in this rob entry.
      DPRINTF(LLVMTraceCPU, "Fill inst %u into rob entry %u\n",
              this->fetchQueue.front(), i);
      this->rob[i].first = this->fetchQueue.front();
      this->rob[i].second = true;
      // Pop from fetchQueue.
      this->fetchQueue.pop();
    }
  }
}

void LLVMTraceCPU::markReadyInst() {
  // Loop through rob and mark ready instructions, until instruction queue is
  // full.
  for (size_t i = 0;
       i < this->rob.size() &&
       this->instructionQueue.size() < this->MAX_INSTRUCTION_QUEUE_SIZE;
       ++i) {
    if (this->rob[i].second) {
      auto instId = this->rob[i].first;
      panic_if(this->inflyInsts.find(instId) == this->inflyInsts.end(),
               "Inst %u should be in inflyInsts to check if READY\n", instId);
      if (this->inflyInsts[instId] != InstStatus::FETCHED) {
        continue;
      }
      // Default set to ready.
      if (this->dynamicInsts[instId]->isDependenceReady(this)) {
        // Mark the status to ready.
        DPRINTF(LLVMTraceCPU,
                "Set inst %u to ready and send to instruction queue.\n",
                instId);
        this->inflyInsts[instId] = InstStatus::READY;
        // Insert to the proper place of the instruction queue.
        auto iter = this->instructionQueue.cbegin();
        auto end = this->instructionQueue.cend();
        while (iter != end) {
          if (instId < *iter) {
            break;
          }
          iter++;
        }
        this->instructionQueue.insert(iter, instId);
        // Mark this rob entry as free.
        this->rob[i].second = false;
      }
    }
  }
}

void LLVMTraceCPU::issue() {
  // Free FUs.
  this->fuPool->processFreeUnits();
  // Loop through the instruction queue.
  for (auto iter = this->instructionQueue.begin(),
            end = this->instructionQueue.end();
       iter != end; ++iter) {
    auto instId = *iter;
    panic_if(this->inflyInsts.find(instId) == this->inflyInsts.end(),
             "Inst %u should be in inflyInsts to check if READY.\n", instId);
    if (this->inflyInsts.at(instId) == InstStatus::READY) {
      bool canIssue = true;
      auto inst = this->dynamicInsts[instId];
      auto opClass = inst->getOpClass();
      auto fuId = FUPool::NoCapableFU;

      // Check if there is available FU.
      if (opClass != No_OpClass) {
        fuId = this->fuPool->getUnit(opClass);
        panic_if(fuId == FUPool::NoCapableFU,
                 "There is no capable FU %s for inst %u.\n",
                 Enums::OpClassStrings[opClass], instId);
        if (fuId == FUPool::NoFreeFU) {
          // We cannot issue for now.
          canIssue = false;
        }
      }

      // Mark it as issued.
      if (canIssue) {
        this->inflyInsts[instId] = InstStatus::ISSUED;
        DPRINTF(LLVMTraceCPU, "inst %u issued\n", instId);
        inst->execute(this);
        this->statIssuedInstType[this->thread_context->threadId()][opClass]++;
        // Handle the FU completion.
        if (opClass != No_OpClass) {
          DPRINTF(LLVMTraceCPU, "Inst %u get FU %s with fuId %d.\n", instId,
                  Enums::OpClassStrings[opClass], fuId);
          // Start the FU FSM for this inst.
          inst->startFUStatusFSM();
          auto opLatency = this->fuPool->getOpLatency(opClass);
          // Special case for only one cycle latency, no need for event.
          if (opLatency == Cycles(1)) {
            // Mark the FU to be completed next cycle.
            this->processFUCompletion(instId, fuId);
          } else {
            // Check if this FU is pipelined.
            bool pipelined = this->fuPool->isPipelined(opClass);
            FUCompletion* fuCompletion =
                new FUCompletion(instId, fuId, this, !pipelined);
            // Schedule the event.
            // Notice for the -1 part so that we can free FU in next cycle.
            this->schedule(fuCompletion,
                           this->clockEdge(Cycles(opLatency - 1)));
            // If pipelined, should the FU be freed immediately.
            this->fuPool->freeUnitNextCycle(fuId);
          }
        }
      }
    }
  }
}

void LLVMTraceCPU::countDownComputeDelay() {
  // Loop through the instruction queue.
  for (auto iter = this->instructionQueue.begin(),
            end = this->instructionQueue.end();
       iter != end; ++iter) {
    auto instId = *iter;
    // Check if issued.
    if (this->inflyInsts.at(instId) != InstStatus::ISSUED) {
      continue;
    }
    // Tick the inst.
    auto inst = this->dynamicInsts[instId];
    inst->tick();

    // Check if the inst is finished.
    if (inst->isCompleted()) {
      // Mark it as finished.
      DPRINTF(LLVMTraceCPU, "Inst %u finished\n", instId);
      this->inflyInsts[instId] = InstStatus::FINISHED;
    }
  }
}

void LLVMTraceCPU::commit() {
  // Loop through the instruction queue.
  // Note that erase will invalidate iter, so we need next.
  auto iter = this->instructionQueue.begin();
  auto end = this->instructionQueue.end();
  while (iter != end) {
    auto next = iter;
    next++;
    auto instId = *iter;
    if (this->inflyInsts.at(instId) == InstStatus::FINISHED) {
      // Erase from instruction queue and inflyInsts.
      this->instructionQueue.erase(iter);
      this->inflyInsts.erase(instId);
      DPRINTF(LLVMTraceCPU, "Inst %u committed, remaining infly inst #%u\n",
              instId, this->inflyInsts.size());
    }

    iter = next;
  }
}

void LLVMTraceCPU::tick() {
  if (curTick() % 1000000 == 0) {
    DPRINTF(LLVMTraceCPU, "Tick()\n");
  }

  // Tick infly inst must come before the commit
  // as some inst will be marked as complete in this cycle.
  countDownComputeDelay();

  // Remember to tick the FUPool to free FUs.

  // Hack: reverse calling them to make sure that no
  // inst get passed through more than 1 stage in one cycle.
  commit();
  issue();
  reorder();
  fetch();

  // Mark inst to be ready for next tick.
  markReadyInst();

  if (this->fetchQueue.empty() && this->inflyInsts.empty()) {
    DPRINTF(LLVMTraceCPU, "We have no inst left to be scheduled.\n");
    // No more need to write to the finish tag, simply restart the
    // normal thread.
    // Check the stack depth should be 0.
    if (this->currentStackDepth != 0) {
      panic("The stack depth should be 0 when we finished a replay, but %u\n",
            this->currentStackDepth);
    }

    DPRINTF(LLVMTraceCPU, "Activate the normal CPU\n");
    this->thread_context->activate();
    return;
  } else {
    // Schedule next Tick event.
    schedule(this->tickEvent, nextCycle());
  }
}

bool LLVMTraceCPU::handleTimingResp(PacketPtr pkt) {
  // Receive the response from port.
  LLVMDynamicInstId instId =
      static_cast<LLVMDynamicInstId>(pkt->req->getReqInstSeqNum());
  if (instId < this->dynamicInsts.size()) {
    this->dynamicInsts[instId]->handlePacketResponse();
  } else {
    panic("Invalid instId %u, max instId %u\n", instId,
          this->dynamicInsts.size());
  }
  // Release the memory.
  delete pkt->req;
  delete pkt;

  return true;
}

void LLVMTraceCPU::CPUPort::sendReq(PacketPtr pkt) {
  // If there is already blocked req, just push to the queue.
  std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (this->blocked) {
    // We are blocked, push to the queue.
    DPRINTF(LLVMTraceCPU, "Already blocked, queue packet ptr %p\n", pkt);
    this->blockedPacketPtrs.push(pkt);
    return;
  }
  // Push to blocked ptrs if need retry, and set blocked flag.
  bool success = MasterPort::sendTimingReq(pkt);
  if (!success) {
    DPRINTF(LLVMTraceCPU, "Blocked packet ptr %p\n", pkt);
    this->blocked = true;
    this->blockedPacketPtrs.push(pkt);
  }
}

void LLVMTraceCPU::CPUPort::recvReqRetry() {
  std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (!this->blocked) {
    panic("Should be in blocked state when recvReqRetry is called\n");
  }
  // Unblock myself.
  this->blocked = false;
  // Keep retry until failed or blocked is empty.
  while (!this->blockedPacketPtrs.empty() && !this->blocked) {
    // If we have blocked packet, send again.
    PacketPtr pkt = this->blockedPacketPtrs.front();
    bool success = MasterPort::sendTimingReq(pkt);
    if (success) {
      DPRINTF(LLVMTraceCPU, "Retry blocked packet ptr %p: Succeed\n", pkt);
      this->blockedPacketPtrs.pop();
    } else {
      DPRINTF(LLVMTraceCPU, "Retry blocked packet ptr %p: Failed\n", pkt);
      this->blocked = true;
    }
  }
}

void LLVMTraceCPU::handleMapVirtualMem(Process* p, ThreadContext* tc,
                                       const std::string& base,
                                       const Addr vaddr) {
  this->mapBaseNameToVAddr(base, vaddr);
}

void LLVMTraceCPU::handleReplay(Process* p, ThreadContext* tc,
                                const std::string& trace,
                                const Addr finish_tag_vaddr) {
  DPRINTF(LLVMTraceCPU, "Replay trace %s, finish tag at 0x%x\n", trace.c_str(),
          finish_tag_vaddr);

  // Set the process and tc.
  this->process = p;
  this->thread_context = tc;

  // Get the bottom of the stack.
  this->stackMin = tc->readIntReg(TheISA::StackPointerReg);

  // Suspend the thread from normal CPU.
  this->thread_context->suspend();
  DPRINTF(LLVMTraceCPU, "Suspend thread, status = %d\n",
          this->thread_context->status());

  if (!this->process->pTable->translate(finish_tag_vaddr,
                                        this->finish_tag_paddr)) {
    panic("Failed translating finish_tag_vaddr %p to paddr\n",
          reinterpret_cast<void*>(finish_tag_vaddr));
  }

  // Update the stack depth to 1.
  if (this->currentStackDepth != 0) {
    panic("Before replay the stack depth must be 0, now %u\n",
          this->currentStackDepth);
  }
  this->currentStackDepth = 1;

  // Schedule the next event.
  schedule(this->tickEvent, nextCycle());
}

Addr LLVMTraceCPU::allocateStack(Addr size, Addr align) {
  // We need to handle stack allocation only
  // when we have a driver.
  if (this->isStandalone()) {
    panic("LLVMTraceCPU::allocateStack called in standalone mode.\n");
  }
  // Allocate the stack starting from stackMin.
  // Note that since we are not acutall modifying the
  // stack pointer in the thread context, there is no
  // clean up necessary when we leaving this function.
  // Compute the bottom of the new stack.
  // Remember to round down to align.
  Addr bottom = roundDown(this->stackMin - size, align);
  // Try to map the bottom to see if there is already
  // a physical page for it.
  Addr paddr;
  if (!this->process->pTable->translate(bottom, paddr)) {
    // We need to allocate more page for the stack.
    if (!this->process->fixupStackFault(bottom)) {
      panic("Failed to allocate stack until %ull\n", bottom);
    }
  }
  // Update the stackMin.
  this->stackMin = bottom;
  return bottom;
}

Addr LLVMTraceCPU::translateAndAllocatePhysMem(Addr vaddr) {
  if (!this->isStandalone()) {
    panic("translateAndAllocatePhysMem called in non standalone mode.\n");
  }

  if (!this->pageTable.translate(vaddr)) {
    // Handle the page fault.
    Addr pageBytes = TheISA::PageBytes;
    Addr startVaddr = this->pageTable.pageAlign(vaddr);
    Addr startPaddr = this->system->allocPhysPages(1);
    this->pageTable.map(startVaddr, startPaddr, pageBytes, PageTableBase::Zero);
    DPRINTF(LLVMTraceCPU, "Map vaddr 0x%x to paddr 0x%x\n", startVaddr,
            startPaddr);
  }
  Addr paddr;
  if (!this->pageTable.translate(vaddr, paddr)) {
    panic("Failed to translate vaddr at 0x%x\n", vaddr);
  }
  DPRINTF(LLVMTraceCPU, "Translate vaddr 0x%x to paddr 0x%x\n", vaddr, paddr);
  return paddr;
}

void LLVMTraceCPU::mapBaseNameToVAddr(const std::string& base, Addr vaddr) {
  DPRINTF(LLVMTraceCPU, "map base %s to vaddr %p.\n", base.c_str(),
          reinterpret_cast<void*>(vaddr));
  this->mapBaseToVAddr[base] = vaddr;
}

// Translate from vaddr to paddr.
Addr LLVMTraceCPU::getPAddrFromVaddr(Addr vaddr) {
  // Translate from vaddr to paddr.
  Addr paddr;
  if (!this->process->pTable->translate(vaddr, paddr)) {
    // Something goes wrong. The simulation process should
    // allocate this address.
    panic("Failed translating vaddr %p to paddr\n",
          reinterpret_cast<void*>(vaddr));
  }
  return paddr;
}

void LLVMTraceCPU::sendRequest(Addr paddr, int size, LLVMDynamicInstId instId,
                               uint8_t* data) {
  RequestPtr req = new Request(paddr, size, 0, this->_dataMasterId, instId,
                               this->thread_context->contextId());
  PacketPtr pkt;
  uint8_t* pkt_data = new uint8_t[req->getSize()];
  if (data == nullptr) {
    pkt = Packet::createRead(req);
  } else {
    pkt = Packet::createWrite(req);
    // Copy the value to store.
    memcpy(pkt_data, data, req->getSize());
  }
  pkt->dataDynamic(pkt_data);
  this->dataPort.sendReq(pkt);
}

LLVMTraceCPU::FUCompletion::FUCompletion(LLVMDynamicInstId _instId, int _fuId,
                                         LLVMTraceCPU* _cpu, bool _shouldFreeFU)
    : Event(Stat_Event_Pri, AutoDelete),
      instId(_instId),
      fuId(_fuId),
      cpu(_cpu),
      shouldFreeFU(_shouldFreeFU) {}

void LLVMTraceCPU::FUCompletion::process() {
  // Call the process function from cpu.
  this->cpu->processFUCompletion(this->instId,
                                 this->shouldFreeFU ? this->fuId : -1);
}

const char* LLVMTraceCPU::FUCompletion::description() const {
  return "Function unit completion";
}

void LLVMTraceCPU::processFUCompletion(LLVMDynamicInstId instId, int fuId) {
  DPRINTF(LLVMTraceCPU, "FUCompletion for inst %u with fu %d\n", instId, fuId);
  // Check if we should free this fu next cycle.
  if (fuId > -1) {
    this->fuPool->freeUnitNextCycle(fuId);
  }
  // Check that this inst is legal and already be issued.
  if (instId >= this->dynamicInsts.size()) {
    panic("processFUCompletion: Illegal inst %u\n", instId);
  }
  if (this->inflyInsts.find(instId) == this->inflyInsts.end()) {
    panic("processFUCompletion: Failed to find the inst %u in inflyInsts\n",
          instId);
  }
  if (this->inflyInsts.at(instId) != InstStatus::ISSUED) {
    panic("processFUCompletion: Inst %u is not issued\n", instId);
  }
  // Inform the inst that it has completed.
  this->dynamicInsts[instId]->handleFUCompletion();
}

void LLVMTraceCPU::regStats() {
  BaseCPU::regStats();

  this->statIssuedInstType.init(numThreads, Enums::Num_OpClass)
      .name(name() + ".FU_type")
      .desc("Type of FU issued")
      .flags(Stats::total | Stats::pdf | Stats::dist);
  this->statIssuedInstType.ysubnames(Enums::OpClassStrings);
}