#include "llvm_trace_cpu.hh"
#include "llvm_trace_cpu_delegator.hh"

#include "base/debug.hh"
#include "base/loader/object_file.hh"
#include "cpu/gem_forge/accelerator/gem_forge_accelerator.hh"
#include "cpu/thread_context.hh"
#include "debug/GemForgeCPUDump.hh"
#include "debug/LLVMTraceCPU.hh"
#include "proto/protoio.hh"
#include "sim/process.hh"
#include "sim/sim_exit.hh"
#include "sim/stat_control.hh"

LLVMTraceCPU::LLVMTraceCPU(LLVMTraceCPUParams *params)
    : BaseCPU(params), cpuParams(params),
      pageTable(params->name + ".page_table", 0, params->system,
                TheISA::PageBytes),
      instPort(params->name + ".inst_port", this),
      dataPort(params->name + ".data_port", this),
      traceFileName(params->traceFile),
      totalActiveCPUs(params->totalActiveCPUs),
      cpuStatus(CPUStatusE::INITIALIZED), fuPool(params->fuPool),
      currentStackDepth(0), initializeMemorySnapshotDone(true),
      warmUpDone(false), cacheWarmer(nullptr), process(nullptr),
      thread_context(nullptr), stackMin(0), fetchStage(params, this),
      decodeStage(params, this), renameStage(params, this),
      iewStage(params, this), commitStage(params, this), fetchToDecode(5, 5),
      decodeToRename(5, 5), renameToIEW(5, 5), iewToCommit(5, 5),
      signalBuffer(5, 5), driver(params->driver), tickEvent(*this) {
  DPRINTF(LLVMTraceCPU, "LLVMTraceCPU constructed\n");

  assert(this->numThreads < LLVMTraceCPUConstants::MaxContexts &&
         "Number of thread context exceed the maximum.");

  // Set the trace folder.
  auto slashPos = this->traceFileName.rfind('/');
  if (slashPos == std::string::npos) {
    this->traceFolder = "";
  } else {
    this->traceFolder = this->traceFileName.substr(0, slashPos);
  }
  this->traceExtraFolder = this->traceFileName + ".extra";

  // Set the time buffer between stages.
  this->fetchStage.setToDecode(&this->fetchToDecode);
  this->decodeStage.setFromFetch(&this->fetchToDecode);
  this->decodeStage.setToRename(&this->decodeToRename);
  this->renameStage.setFromDecode(&this->decodeToRename);
  this->renameStage.setToIEW(&this->renameToIEW);
  this->iewStage.setFromRename(&this->renameToIEW);
  this->iewStage.setToCommit(&this->iewToCommit);
  this->commitStage.setFromIEW(&this->iewToCommit);

  this->commitStage.setSignal(&this->signalBuffer, 0);
  this->iewStage.setSignal(&this->signalBuffer, -1);
  this->renameStage.setSignal(&this->signalBuffer, -2);
  this->decodeStage.setSignal(&this->signalBuffer, -3);
  this->fetchStage.setSignal(&this->signalBuffer, -4);

  // Initialize the hardware contexts.
  this->activeThreads.resize(params->hardwareContexts, nullptr);

  this->runTimeProfiler = new RunTimeProfiler();

  if (!this->isStandalone()) {
    // Handshake with the driver.
    driver->handshake(this);
    panic("Standalone mode not supported any more.");
    return;
  }

  /************************************
   * No driver, standalone mode.
   ************************************/

  // First check if I am active.
  if (this->traceFileName.empty()) {
    // No trace assigned to me, I am inactive.
    // Simply done.
    return;
  }

  // Initialize the main thread.
  auto mainThreadId = LLVMTraceCPU::allocateContextID();
  this->mainThread = new LLVMTraceThreadContext(
      mainThreadId, this->traceFileName, false /*isIdeal */,
      this->cpuParams->adfaEnable);
  if (this->mainThread->getRegionStats() != nullptr) {
    // Add the dump handler to dump region stats at the end.
    Stats::registerDumpCallback(
        new MakeCallback<RegionStats, &RegionStats::dump>(
            this->mainThread->getRegionStats(), true));
  }
  this->activateThread(mainThread);

  // Reset the initializeMemorySnapshotDone so that we will initialize the
  // memory.
  this->initializeMemorySnapshotDone = !params->installMemorySnapshot;

  // Initialize the cache warmer.
  if (this->cpuParams->warmCache) {
    this->cacheWarmer =
        new CacheWarmer(this, this->traceFileName + ".cache", 1);
  }

  // Schedule the first event.
  // And remember to initialize the stack depth to 1.
  this->currentStackDepth = 1;
  DPRINTF(LLVMTraceCPU, "Schedule initial tick event.\n");
  schedule(this->tickEvent, nextCycle());
}

LLVMTraceCPU::~LLVMTraceCPU() {
  delete this->runTimeProfiler;
  this->runTimeProfiler = nullptr;
  delete this->mainThread;
  this->mainThread = nullptr;
}

LLVMTraceCPU *LLVMTraceCPUParams::create() { return new LLVMTraceCPU(this); }

void LLVMTraceCPU::init() {
  BaseCPU::init();
  this->cpuDelegator = m5::make_unique<LLVMTraceCPUDelegator>(this);
  if (this->accelManager) {
    // Create the delegator and handshake with the accelerator manager.
    this->accelManager->handshake(this->cpuDelegator.get());
  }
}

void LLVMTraceCPU::tick() {

  // Remember to initialize the memory.
  if (!this->initializeMemorySnapshotDone) {
    this->initializeMemorySnapshot();
    this->initializeMemorySnapshotDone = true;
  }

  /**
   * Now we are warm up in timing mode,
   * so we always need to send the packets.
   */
  this->dataPort.sendReq();

  this->numCycles++;

  if (this->cacheWarmer != nullptr) {
    // Disable all tracing output.
    Debug::SimpleFlag::disableAll();
    if (this->cacheWarmer->isDoneWithPreviousRequest()) {
      if (this->cacheWarmer->isDone()) {
        // We are done warming up.
        inform("Done cache warmup, %lu.\n", this->numCycles.value());
        delete this->cacheWarmer;
        this->cacheWarmer = nullptr;
        this->warmUpDone = true;
        // Reset the stats.
        Stats::reset();
        inform("Done reset, %lu.\n", this->numCycles.value());
        this->system->incWorkItemsEnd();
        this->cpuStatus = CPUStatusE::CACHE_WARMED;
      } else {
        // We can try a new packet.
        auto pkt = this->cacheWarmer->getNextWarmUpPacket();
        assert(pkt != nullptr && "Should not a null warm up packet.");
        // There are still more packets.
        this->sendRequest(pkt);
        this->cpuStatus = CPUStatusE::CACHE_WARMING;
      }
    }
    // We schedule the next
    schedule(this->tickEvent, nextCycle());
    return;
  }

  if (this->cpuStatus == CPUStatusE::CACHE_WARMED) {
    // We want to synchronize all cpus here.
    if (this->system->getWorkItemsEnd() % this->totalActiveCPUs == 0) {
      // We should have been synchronized.
      Debug::SimpleFlag::enableAll();
      this->cpuStatus = CPUStatusE::EXECUTING;
      inform("Core %d start executing.\n", this->cpuId());
    } else {
      // Check next cycle.
      schedule(this->tickEvent, nextCycle());
      return;
    }
  }

  if (Debug::GemForgeCPUDump) {
    if (curTick() % 100000000 == 0) {
      DPRINTF(LLVMTraceCPU, "Tick()\n");
      this->iewStage.dumpROB();
      if (this->accelManager) {
        this->accelManager->dump();
      }
    }
  }

  // Try to parse more instructions.
  for (auto thread : this->activeThreads) {
    thread->parse();
  }

  // Unblock the memory instructions.
  if (!this->dataPort.isBlocked()) {
    this->iewStage.unblockMemoryInsts();
  }

  this->fetchStage.tick();
  this->decodeStage.tick();
  this->renameStage.tick();
  this->iewStage.tick();
  // this->accelManager->tick();
  this->commitStage.tick();

  this->fetchToDecode.advance();
  this->decodeToRename.advance();
  this->renameToIEW.advance();
  this->iewToCommit.advance();
  this->signalBuffer.advance();

  // Exit condition.
  // 1. In standalone mode, we will exit when there is no infly instructions and
  //    the loaded instruction list is empty.
  // 2. In integrated mode, we will exit when there is no infly instructions and
  //    the stack depth is 0.
  bool done = false;
  if (this->isStandalone()) {
    if (this->cpuParams->max_insts_any_thread > 0) {
      auto &committedInsts = this->commitStage.instsCommitted;
      for (int i = 0; i < committedInsts.size(); ++i) {
        if (committedInsts[i].value() > this->cpuParams->max_insts_any_thread) {
          done = true;
          break;
        }
      }
    } else {
      done = (this->getNumActiveThreads() == 0);
      if (done) {
        assert(this->inflyInstStatus.empty() &&
               "Infly instruction status map is not empty when done.");
      }
    }
  } else {
    done = this->inflyInstStatus.empty() && this->currentStackDepth == 0;
  }
  if (done) {
    DPRINTF(LLVMTraceCPU, "We have no inst left to be scheduled.\n");
    // Wraps up the region stats by sending in the invalid bb.
    auto regionStats = this->mainThread->getRegionStats();
    if (regionStats) {
      regionStats->update(RegionStats::InvalidBB);
    }
    // If in standalone mode, we can exit.
    if (this->isStandalone()) {
      // Decrease the workitem count.
      auto workItemsEnd = this->system->incWorkItemsEnd();
      if (workItemsEnd % this->totalActiveCPUs == 0) {
        if (regionStats != nullptr) {
          regionStats->dump();
        }
        this->runTimeProfiler->dump("profile.txt");
        exitSimLoop("All datagraphs finished.\n");
      } else {
        DPRINTF(LLVMTraceCPU, "CPU %d done.\n", this->_cpuId);
      }

    } else {
      DPRINTF(LLVMTraceCPU, "Activate the normal CPU\n");
      this->thread_context->activate();
    }
    // Do not schedule next tick.
    return;
  } else {
    // Schedule next Tick event.
    schedule(this->tickEvent, nextCycle());
  }

  this->numPendingAccessDist.sample(this->dataPort.getPendingPacketsNum());
}

void LLVMTraceCPU::initializeMemorySnapshot() {
  assert(this->isStandalone() &&
         "Only need to initialize memory snapshot in standalone mode.");
  auto snapshotFileName = this->traceExtraFolder + "/mem.snap";
  ProtoInputStream snapshotIStream(snapshotFileName);
  LLVM::TDG::MemorySnapshot snapshot;
  assert(snapshotIStream.read(snapshot) &&
         "Failed to read in the memory snapshot.");
  /**
   * Do functional access.
   */
  PortProxy proxy(this->dataPort, this->system->cacheLineSize());
  Addr vaddr;
  for (const auto &AddrValuePair : snapshot.snapshot()) {
    vaddr = AddrValuePair.first;
    const auto &Value = AddrValuePair.second;
    // * Dummy way to write byte by byte so that we do not have to align with
    // * pages.
    for (size_t idx = 0; idx < Value.size(); ++idx) {
      auto paddr = this->translateAndAllocatePhysMem(vaddr + idx);
      // Store use proxy.
      proxy.writeBlob(paddr,
                      reinterpret_cast<const uint8_t *>(Value.data() + idx), 1);
    }
    // const Addr interestAddr = 0x88707c;
    // for (Addr x = interestAddr; x < interestAddr + 4; ++x) {
    //   if (x >= vaddr && x < vaddr + Value.size()) {
    //     hack("Stored Addr 0x%x byte %u.\n", x, Value.at(x - vaddr));
    //   }
    // }
  }
  inform("MemorySnapshot initialized.\n");
}

bool LLVMTraceCPU::handleTimingResp(PacketPtr pkt) {
  // Receive the response from port.
  GemForgePacketHandler::handleGemForgePacketResponse(this->getDelegator(),
                                                      pkt);
  return true;
}

LLVMTraceCPU::CacheWarmer::CacheWarmer(LLVMTraceCPU *_cpu,
                                       const std::string &fn,
                                       size_t _repeatedTimes)
    : cpu(_cpu), repeatedTimes(_repeatedTimes), warmUpAddrs(0),
      receivedPackets(0) {
  ProtoInputStream protoInput(fn);
  assert(protoInput.read(this->cacheWarmUpProto) &&
         "Failed to readin the cache warm file.");
}

PacketPtr LLVMTraceCPU::CacheWarmer::getNextWarmUpPacket() {
  assert(!isDone() && "Already done.");
  if (this->warmUpAddrs ==
      this->cacheWarmUpProto.requests_size() * this->repeatedTimes) {
    // No more packets.
    return nullptr;
  }
  const auto &request = this->cacheWarmUpProto.requests(
      this->warmUpAddrs % this->cacheWarmUpProto.requests_size());
  this->warmUpAddrs++;

  const auto vaddr = request.addr();
  auto size = request.size();
  const auto pc = request.pc();

  // Truncate by cache line.
  const auto cacheLineSize = this->cpu->system->cacheLineSize();
  if ((vaddr % cacheLineSize) + size > cacheLineSize) {
    size = cacheLineSize - (vaddr % cacheLineSize);
  }

  auto paddr = this->cpu->translateAndAllocatePhysMem(vaddr);

  // if (this->warmUpAddrs % 2 == 0) {
  //   hack("Generate warmup for %d %#x %d. ", this->warmUpAddrs, paddr >> 12,
  //        (paddr >> 12) & 0x3);
  // } else {
  //   hack("Generate warmup for %d %#x %d.\n", this->warmUpAddrs, paddr >> 12,
  //        (paddr >> 12) & 0x3);
  // }
  auto pkt = GemForgePacketHandler::createGemForgePacket(
      paddr, size, this, nullptr, this->cpu->dataMasterId(), 0, pc);
  return pkt;
}

void LLVMTraceCPU::CacheWarmer::handlePacketResponse(
    GemForgeCPUDelegator *cpuDelegator, PacketPtr packet) {
  // Just release the packet.
  this->receivedPackets++;
  // hack("Received warmup for %d.\n", this->receivedPackets);
  delete packet;
}

bool LLVMTraceCPU::CPUPort::recvTimingResp(PacketPtr pkt) {
  if (this->inflyNumPackets == 0) {
    panic("Received timing response when there is no infly packets.");
  }
  this->inflyNumPackets--;
  return this->owner->handleTimingResp(pkt);
}

void LLVMTraceCPU::CPUPort::addReq(PacketPtr pkt) {
  DPRINTF(LLVMTraceCPU, "Add pkt at %p\n", pkt);
  this->blockedPacketPtrs.push(pkt);
  // Try to send request.
  this->sendReq();
}

bool LLVMTraceCPU::CPUPort::isBlocked() const {
  return this->blocked || (!this->blockedPacketPtrs.empty());
}

void LLVMTraceCPU::CPUPort::sendReq() {
  /**
   * At this level, we do not distinguish the load/store ports,
   * but only enfore the limit on the total number of ports.
   */
  unsigned usedPorts = 0;
  this->owner->numOutstandingAccessDist.sample(this->inflyNumPackets);
  const auto cpuParams = this->owner->getLLVMTraceCPUParams();
  unsigned totalPorts = cpuParams->cacheLoadPorts + cpuParams->cacheStorePorts;
  while (!this->blocked && !this->blockedPacketPtrs.empty() &&
         usedPorts < totalPorts && this->inflyNumPackets < 80) {
    PacketPtr pkt = this->blockedPacketPtrs.front();
    DPRINTF(LLVMTraceCPU, "Try sending pkt at %p\n", pkt);
    bool success = MasterPort::sendTimingReq(pkt);
    if (!success) {
      DPRINTF(LLVMTraceCPU, "%llu Blocked packet ptr %p\n",
              this->owner->curCycle(), pkt);
      this->blocked = true;
    } else {
      // Only increase the inflyNumPackets if it needs a response.
      if (::GemForgePacketHandler::needResponse(pkt)) {
        this->inflyNumPackets++;
      }
      this->blockedPacketPtrs.pop();
      ::GemForgePacketHandler::issueToMemory(this->owner->getDelegator(), pkt);
      usedPorts++;
    }
  }
}

void LLVMTraceCPU::CPUPort::recvReqRetry() {
  // std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (!this->blocked) {
    panic("Should be in blocked state when recvReqRetry is called\n");
  }
  // Unblock myself.
  this->blocked = false;
  // Keep retry until failed or blocked is empty.
  this->sendReq();
}

void LLVMTraceCPU::handleReplay(
    Process *p, ThreadContext *tc, const std::string &trace,
    const Addr finish_tag_vaddr,
    std::vector<std::pair<std::string, Addr>> maps) {
  panic_if(this->isStandalone(), "handleReplay called in standalone mode.");

  DPRINTF(LLVMTraceCPU, "Replay trace %s, finish tag at 0x%x, num maps %u\n",
          trace.c_str(), finish_tag_vaddr, maps.size());

  // Map base to vaddr.
  for (const auto &pair : maps) {
    this->mapBaseNameToVAddr(pair.first, pair.second);
  }

  // Set the process and tc.
  this->process = p;
  this->thread_context = tc;

  // Load the global symbols for global variables.
  this->process->objFile->loadAllSymbols(&this->symbol_table);

  // Get the bottom of the stack.
  this->stackMin = tc->readIntReg(TheISA::StackPointerReg);

  // Allocate a special stack slot for register spill.
  Addr spill = this->allocateStack(8, 8);
  this->mapBaseNameToVAddr("$sp", spill);

  // Suspend the thread from normal CPU.
  this->thread_context->suspend();
  DPRINTF(LLVMTraceCPU, "Suspend thread, status = %d\n",
          this->thread_context->status());

  if (!this->process->pTable->translate(finish_tag_vaddr,
                                        this->finish_tag_paddr)) {
    panic("Failed translating finish_tag_vaddr %p to paddr\n",
          reinterpret_cast<void *>(finish_tag_vaddr));
  }

  // Update the stack depth to 1.
  if (this->currentStackDepth != 0) {
    panic("Before replay the stack depth must be 0, now %u\n",
          this->currentStackDepth);
  }
  this->stackPush();

  // Schedule the next event.
  schedule(this->tickEvent, nextCycle());
}

LLVMDynamicInst *LLVMTraceCPU::getInflyInst(LLVMDynamicInstId id) {
  auto iter = this->inflyInstMap.find(id);
  panic_if(iter == this->inflyInstMap.end(), "Failed to find infly inst %u.\n",
           id);
  return iter->second;
}

LLVMDynamicInst *LLVMTraceCPU::getInflyInstNullable(LLVMDynamicInstId id) {
  auto iter = this->inflyInstMap.find(id);
  if (iter == this->inflyInstMap.end()) {
    return nullptr;
  }
  return iter->second;
}

void LLVMTraceCPU::stackPush() {
  // Ignore the stack adjustment if we are in standalone mode.
  if (this->isStandalone()) {
    return;
  }
  this->currentStackDepth++;
  this->framePointerStack.push_back(this->stackMin);
}

void LLVMTraceCPU::stackPop() {
  if (this->isStandalone()) {
    return;
  }
  this->currentStackDepth--;
  if (this->currentStackDepth < 0) {
    panic("Current stack depth is less than 0\n");
  }
  this->stackMin = this->framePointerStack.back();
  this->framePointerStack.pop_back();
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
    this->pageTable.map(startVaddr, startPaddr, pageBytes);
    DPRINTF(LLVMTraceCPU, "Map vaddr 0x%x to paddr 0x%x\n", startVaddr,
            startPaddr);
  }
  Addr paddr;
  if (!this->pageTable.translate(vaddr, paddr)) {
    panic("Failed to translate vaddr at 0x%x\n", vaddr);
  }
  return paddr;
}

void LLVMTraceCPU::mapBaseNameToVAddr(const std::string &base, Addr vaddr) {
  DPRINTF(LLVMTraceCPU, "map base %s to vaddr %p.\n", base.c_str(),
          reinterpret_cast<void *>(vaddr));
  this->mapBaseToVAddr[base] = vaddr;
}

Addr LLVMTraceCPU::getVAddrFromBase(const std::string &base) {
  if (this->mapBaseToVAddr.find(base) != this->mapBaseToVAddr.end()) {
    return this->mapBaseToVAddr.at(base);
  }
  // Try to look at the global symbol table of the process.
  Addr vaddr;
  if (this->symbol_table.findAddress(base, vaddr)) {
    return vaddr;
  }
  panic("Failed to look up base %s\n", base.c_str());
}

// Translate from vaddr to paddr.
Addr LLVMTraceCPU::getPAddrFromVaddr(Addr vaddr) {
  // Translate from vaddr to paddr.
  Addr paddr;
  if (!this->process->pTable->translate(vaddr, paddr)) {
    // Something goes wrong. The simulation process should
    // allocate this address.
    panic("Failed translating vaddr %p to paddr\n",
          reinterpret_cast<void *>(vaddr));
  }
  return paddr;
}

void LLVMTraceCPU::sendRequest(PacketPtr pkt) {
  // int contextId = 0;
  // if (!this->isStandalone()) {
  //   contextId = this->thread_context->contextId();
  // }
  this->dataPort.addReq(pkt);
}

Cycles LLVMTraceCPU::getOpLatency(OpClass opClass) {
  if (opClass == No_OpClass) {
    return Cycles(1);
  }
  return this->fuPool->getOpLatency(opClass);
}

void LLVMTraceCPU::regStats() {
  BaseCPU::regStats();

  this->fetchStage.regStats();
  this->decodeStage.regStats();
  this->renameStage.regStats();
  this->iewStage.regStats();
  this->commitStage.regStats();

  this->numPendingAccessDist.init(0, 4, 1)
      .name(this->name() + ".pending_acc_per_cycle")
      .desc("Number of pending memory access each cycle")
      .flags(Stats::pdf);
  this->numOutstandingAccessDist.init(0, 16, 2)
      .name(this->name() + ".outstanding_acc_per_cycle")
      .desc("Number of outstanding memory access each cycle")
      .flags(Stats::pdf);
}

ContextID LLVMTraceCPU::allocateContextID() {
  static ContextID contextId = 0;
  return contextId++;
}

void LLVMTraceCPU::activateThread(LLVMTraceThreadContext *thread) {
  auto freeContextId = -1;
  for (auto idx = 0; idx < this->activeThreads.size(); ++idx) {
    if (this->activeThreads[idx] == nullptr) {
      freeContextId = idx;
      break;
    }
  }
  assert(freeContextId != -1 &&
         "Failed to find free hardware context to activate thread.");
  this->activeThreads[freeContextId] = thread;
  thread->activate(this, freeContextId);
}

void LLVMTraceCPU::deactivateThread(LLVMTraceThreadContext *thread) {
  auto threadId = thread->getThreadId();
  assert(threadId >= 0 && threadId < this->activeThreads.size() &&
         "Invalid context id.");
  assert(this->activeThreads[threadId] == thread &&
         "Unmatched thread at the context.");
  thread->deactivate();
  this->activeThreads[threadId] = nullptr;
  this->fetchStage.clearThread(threadId);
  this->decodeStage.clearThread(threadId);
  this->renameStage.clearThread(threadId);
  this->iewStage.clearThread(threadId);
  this->commitStage.clearThread(threadId);
}

size_t LLVMTraceCPU::getNumActiveThreads() const {
  size_t activeThreads = 0;
  for (const auto &thread : this->activeThreads) {
    if (thread != nullptr) {
      activeThreads++;
    }
  }
  return activeThreads;
}

size_t LLVMTraceCPU::getNumActiveNonIdealThreads() const {
  size_t activeThreads = 0;
  for (const auto &thread : this->activeThreads) {
    if (thread != nullptr) {
      if (!thread->isIdealThread()) {
        activeThreads++;
      }
    }
  }
  return activeThreads;
}