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
      traceFile(params->traceFile),
      currentStackDepth(0),
      currentInstId(0),
      process(nullptr),
      thread_context(nullptr),
      stackMin(0),
      tickEvent(*this),
      MAX_FETCH_QUEUE_SIZE(params->maxFetchQueueSize),
      MAX_REORDER_BUFFER_SIZE(params->maxReorderBufferSize),
      driver(params->driver) {
  DPRINTF(LLVMTraceCPU, "LLVMTraceCPU constructed\n");
  // Initialize the rob buffer.
  this->rob = std::vector<std::pair<DynamicInstId, bool>>(
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
    schedule(this->tickEvent, curTick() + 1);
  }
}

LLVMTraceCPU::~LLVMTraceCPU() {}

LLVMTraceCPU* LLVMTraceCPUParams::create() { return new LLVMTraceCPU(this); }

namespace {
/**
 * Split a string like a|b|c| into [a, b, c].
 */
std::vector<std::string> splitByChar(const std::string& source, char split) {
  std::vector<std::string> ret;
  for (size_t idx = 0, prev = 0; idx < source.size(); ++idx) {
    if (source[idx] == split) {
      ret.push_back(source.substr(prev, idx - prev));
      prev = idx + 1;
    }
  }
  return std::move(ret);
}
}  // namespace

void LLVMTraceCPU::readTraceFile() {
  std::ifstream stream(this->traceFile);
  if (!stream.is_open()) {
    fatal("Failed opening trace file %s\n", this->traceFile.c_str());
  }
  for (std::string line; std::getline(stream, line);) {
    DPRINTF(LLVMTraceCPU, "read in %s\n", line.c_str());
    auto fields = splitByChar(line, '|');
    // Default is compute type.
    DynamicInst::Type type = DynamicInst::Type::COMPUTE;
    Tick delay = std::stoul(fields[1]);
    size_t dependentIdIndex = 2;
    // Fields for LOAD/STORE.
    int size = -1;
    std::string base = "";
    Addr offset = 0x0;
    Addr trace_vaddr = 0x0;
    // Fields for STORE.
    uint8_t* value = nullptr;
    // Hack for align.
    Addr align = 8;

    if (fields[0] == "s") {
      type = DynamicInst::Type::STORE;
      base = fields[2];
      offset = stoull(fields[3]);
      trace_vaddr = stoull(fields[4]);
      size = stoi(fields[5]);
      // Handle the value of store operation.
      int typeId = stoi(fields[6]);
      switch (typeId) {
        case 3: {
          // Double type.
          value = (uint8_t*)(new double);
          *((double*)value) = stod(fields[8]);
          break;
        }
        case 11: {
          // Arbitrary bit width integer. Check the type name.
          if (fields[7] == "i64") {
            value = (uint8_t*)(new uint64_t);
            *((uint64_t*)value) = stoull(fields[8]);
          } else if (fields[7] == "i32") {
            value = (uint8_t*)(new uint32_t);
            *((uint32_t*)value) = stoul(fields[8]);
          } else if (fields[7] == "i8") {
            value = new uint8_t;
            *((uint8_t*)value) = stoul(fields[8]);
          } else {
            fatal("Unsupported integer type %s\n", fields[7].c_str());
          }
          break;
        }
        case 16: {
          // Vector.
          // TODO: Refactor this type extracting code and make it
          // more safe.
          // auto x_pos = fields[7].find('x');
          // if (x_pos == std::string::npos) {
          //   fatal("Invalud vector type %s\n", fields[7].c_str());
          // }
          // size_t num_element = stoul(fields[])
          uint8_t* buffer = new uint8_t[size];
          size_t idx = 0;
          for (auto b : splitByChar(fields[8], ',')) {
            if (idx >= size) {
              fatal(
                  "Number of bytes exceeds the size %u of the vector, content "
                  "%s\n",
                  size, fields[8].c_str());
            }
            // Parse the vector.
            buffer[idx++] = (uint8_t)(stoul(b) & 0xFF);
          }
          if (idx != size) {
            fatal("Number of bytes not equal to the size %u, content %s\n",
                  size, fields[8].c_str());
          }
          value = buffer;
          break;
        }
        default:
          fatal("Unsupported type id %d\n", typeId);
      }
      dependentIdIndex = 9;
    } else if (fields[0] == "l") {
      type = DynamicInst::Type::LOAD;
      base = fields[2];
      offset = stoull(fields[3]);
      trace_vaddr = stoull(fields[4]);
      size = stoi(fields[5]);
      dependentIdIndex = 6;
    } else if (fields[0] == "alloca") {
      // Alloca inst.
      type = DynamicInst::Type::ALLOCA;
      base = fields[2];
      trace_vaddr = stoull(fields[3]);
      size = stoi(fields[4]);
      // A hack for the fft-transpose as we know
      // the alignment is 16.
      align = 16;
      dependentIdIndex = 5;
    } else if (fields[0] == "call") {
      type = DynamicInst::Type::CALL;
    } else if (fields[0] == "ret") {
      type = DynamicInst::Type::RET;
    } else if (fields[0] == "sin") {
      type = DynamicInst::Type::SIN;
    } else if (fields[0] == "cos") {
      type = DynamicInst::Type::COS;
    }

    // Load the tailing dependent inst ids.
    std::vector<DynamicInstId> dependentInstIds;
    while (dependentIdIndex < fields.size()) {
      if (fields[dependentIdIndex] != "") {
        // Empty string may happen when there is no dependence.
        dependentInstIds.push_back(std::stoull(fields[dependentIdIndex]));
      }
      dependentIdIndex++;
    }

    this->dynamicInsts.emplace_back(type, delay, std::move(dependentInstIds),
                                    size, base, offset, trace_vaddr, value,
                                    align);
    DPRINTF(LLVMTraceCPU, "Parsed #%u dynamic inst with %s\n",
            this->dynamicInsts.size(),
            this->dynamicInsts.back().toLine().c_str());
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
    auto instType = this->dynamicInsts[this->currentInstId].type;
    if (instType == DynamicInst::Type::CALL) {
      this->currentStackDepth++;
      DPRINTF(LLVMTraceCPU, "Stack depth ++ to %u\n", this->currentStackDepth);
    } else if (instType == DynamicInst::Type::RET) {
      if (this->currentStackDepth == 0) {
        panic("Current stack depth is already 0, cannot do --\n");
      }
      this->currentStackDepth--;
      DPRINTF(LLVMTraceCPU, "Stack depth -- to %u\n", this->currentStackDepth);
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
  // Loop through rob and mark ready instructions.
  for (size_t i = 0; i < this->rob.size(); ++i) {
    if (this->rob[i].second) {
      DynamicInstId instId = this->rob[i].first;
      panic_if(this->inflyInsts.find(instId) == this->inflyInsts.end(),
               "Inst %u should be in inflyInsts to check if READY\n", instId);
      if (this->inflyInsts[instId] != InstStatus::FETCHED) {
        continue;
      }
      // Default set to ready.
      bool ready = true;
      for (auto dependentInstId : this->dynamicInsts[instId].dependentInstIds) {
        if (this->inflyInsts.find(dependentInstId) != this->inflyInsts.end()) {
          // This inst is still not ready.
          ready = false;
          break;
        }
      }
      if (ready) {
        // Mark the status to ready.
        DPRINTF(LLVMTraceCPU, "Set inst %u to ready\n", instId);
        this->inflyInsts[instId] = InstStatus::READY;
      }
    }
  }
}

void LLVMTraceCPU::issue() {
  for (size_t i = 0; i < this->rob.size(); ++i) {
    if (this->rob[i].second) {
      DynamicInstId instId = this->rob[i].first;
      panic_if(this->inflyInsts.find(instId) == this->inflyInsts.end(),
               "Inst %u should be in inflyInsts to check if READY\n", instId);

      // Check if ready.
      if (this->inflyInsts[instId] != InstStatus::READY) {
        continue;
      }

      // Mark it as issued.
      this->inflyInsts[instId] = InstStatus::ISSUED;
      DPRINTF(LLVMTraceCPU, "inst %u issued\n", instId);

      DynamicInst& inst = this->dynamicInsts[instId];

      switch (inst.type) {
        case DynamicInst::Type::CALL:
        case DynamicInst::Type::RET:
        case DynamicInst::Type::SIN:
        case DynamicInst::Type::COS:
        case DynamicInst::Type::COMPUTE: {
          // Just do nothing.
          break;
        }
        case DynamicInst::Type::ALLOCA: {
          // We need to handle stack allocation only
          // when we have a driver.
          if (this->driver != nullptr) {
            // Allocate the stack starting from stackMin.
            // Note that since we are not acutall modifying the
            // stack pointer in the thread context, there is no
            // clean up necessary when we leaving this function.
            // Compute the bottom of the new stack.
            // Remember to round down to align.
            Addr bottom = roundDown(this->stackMin - inst.size, inst.align);
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
            // Set up the mapping.
            DPRINTF(LLVMTraceCPU, "Allocate stack for %s at %p\n",
                    inst.base.c_str(), (void*)bottom);
            this->mapBaseToVAddr[inst.base] = bottom;
          }
          break;
        }
        case DynamicInst::Type::LOAD:
        case DynamicInst::Type::STORE: {
          // Debug hack, no memory access.
          // if (instId > 53596) {
          //   this->inflyInsts[instId] = InstStatus::FINISHED;
          //   break;
          // }
          for (int packetSize, inflyPacketsSize = 0;
               inflyPacketsSize < inst.size; inflyPacketsSize += packetSize) {
            Addr paddr, vaddr;
            if (this->driver == nullptr) {
              // When in stand alone mode, use the trace space address
              // directly as the virtual address.
              vaddr = inst.trace_vaddr + inflyPacketsSize;
              paddr = this->translateAndAllocatePhysMem(vaddr);
            } else {
              // When we have a driver, we have to translate trace space
              // address into simulation space and then use the process
              // page table to get physical address.
              paddr = this->translateTraceToPhysMem(
                  inst.base, inst.offset + inflyPacketsSize);
              vaddr = this->mapBaseToVAddr[inst.base] + inst.offset +
                      inflyPacketsSize;
            }
            // For now only support maximum 16 bytes access.
            packetSize = (inst.size - inflyPacketsSize);
            if (packetSize > 16) {
              packetSize = 16;
            }
            RequestPtr req =
                new Request(paddr, packetSize, 0, this->_dataMasterId, instId,
                            this->thread_context->contextId());
            PacketPtr pkt;
            uint8_t* pkt_data = new uint8_t[req->getSize()];
            if (inst.type == DynamicInst::Type::LOAD) {
              pkt = Packet::createRead(req);
            } else {
              pkt = Packet::createWrite(req);
              // Copy the value to store.
              memcpy(pkt_data, inst.value + inflyPacketsSize, req->getSize());
            }
            pkt->dataDynamic(pkt_data);
            DPRINTF(LLVMTraceCPU,
                    "Send request %d vaddr %p paddr %p size %u for inst %d\n",
                    inst.numInflyPackets, (void*)vaddr, (void*)paddr,
                    req->getSize(), instId);
            this->dataPort.sendReq(pkt);
            // Update infly packets number.
            inst.numInflyPackets++;
          }
          break;
        }
        default: { panic("Unknown dynamic instruction type %u\n", inst.type); }
      }
    }
  }
}

void LLVMTraceCPU::countDownComputeDelay() {
  for (size_t i = 0; i < this->rob.size(); ++i) {
    if (this->rob[i].second) {
      DynamicInstId instId = this->rob[i].first;
      // Check if issued.
      if (this->inflyInsts[instId] != InstStatus::ISSUED) {
        continue;
      }
      // Check if is compute inst.
      // TODO: Fix this stupid stuff.
      DynamicInst& inst = this->dynamicInsts[instId];
      if (inst.type != DynamicInst::Type::COMPUTE &&
          inst.type != DynamicInst::Type::CALL &&
          inst.type != DynamicInst::Type::RET &&
          inst.type != DynamicInst::Type::SIN &&
          inst.type != DynamicInst::Type::COS &&
          inst.type != DynamicInst::Type::ALLOCA) {
        continue;
      }

      panic_if(inst.computeDelay <= 0,
               "inst %u has non positive compute delay!", instId);
      inst.computeDelay--;
      if (inst.computeDelay == 0) {
        // Mark this inst as finished.
        DPRINTF(LLVMTraceCPU, "Inst %u finished\n", instId);
        this->inflyInsts[instId] = InstStatus::FINISHED;
      }
    }
  }
}

void LLVMTraceCPU::commit() {
  for (size_t i = 0; i < this->rob.size(); ++i) {
    if (this->rob[i].second) {
      DynamicInstId instId = this->rob[i].first;
      // Check if finished.
      if (this->inflyInsts[instId] != InstStatus::FINISHED) {
        continue;
      }
      // Mark the rob entry as free again.
      this->rob[i].second = false;
      // Erase it from the inflyInsts.
      this->inflyInsts.erase(instId);
      DPRINTF(LLVMTraceCPU, "Inst %u committed, remaining infly inst #%u\n",
              instId, this->inflyInsts.size());
    }
  }
}

void LLVMTraceCPU::tick() {
  if (curTick() % 1000000 == 0) {
    DPRINTF(LLVMTraceCPU, "Tick()\n");
  }

  // HACK: CHECK 0xf7a352f0.
  // {
  //   Addr paddr;
  //   if (!this->process->pTable->translate(0xf7a352f0, paddr)) {
  //     panic("Failed to translate 0xf7a352f0\n");
  //   }
  // }

  fetch();
  reorder();
  issue();
  commit();
  // Mark inst to be ready for next tick.
  markReadyInst();
  // Compute down the compute delay.
  countDownComputeDelay();

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
    schedule(this->tickEvent, curTick() + 1);
  }
}

bool LLVMTraceCPU::handleTimingResp(PacketPtr pkt) {
  // Receive the response from port.
  // Special case: this is a write packet to the finish_tag.
  // Do not schedule next event.
  DynamicInstId instId = pkt->req->getReqInstSeqNum();
  if (instId < this->dynamicInsts.size()) {
    DynamicInst& inst = this->dynamicInsts[instId];
    DPRINTF(LLVMTraceCPU, "Get response for inst %u, remain infly packets %d\n",
            instId, --(inst.numInflyPackets));
    panic_if(this->inflyInsts.find(instId) == this->inflyInsts.end(),
             "inst %u should be in inflyInsts\n", instId);
    if (inst.numInflyPackets == 0) {
      panic_if(this->inflyInsts[instId] != InstStatus::ISSUED,
               "inst %u should be issued to be finished\n", instId);
      this->inflyInsts[instId] = InstStatus::FINISHED;
      DPRINTF(LLVMTraceCPU, "Finish inst %u\n", instId);
    }
  } else {
    panic("Invalid instId %u, max instId %u\n", instId,
          this->dynamicInsts.size());
  }
  // Release the memory.
  delete pkt->req;
  delete pkt;

  return true;
}

Addr LLVMTraceCPU::translateAndAllocatePhysMem(Addr vaddr) {
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

Addr LLVMTraceCPU::translateTraceToPhysMem(const std::string& base,
                                           Addr offset) {
  // Check the base address.
  if (this->mapBaseToVAddr.find(base) == this->mapBaseToVAddr.end()) {
    panic("Unknown base name %s\n", base.c_str());
  }
  Addr base_vaddr = this->mapBaseToVAddr[base];
  Addr simu_vaddr = base_vaddr + offset;

  // Translate from vaddr to paddr.
  Addr paddr;
  if (!this->process->pTable->translate(simu_vaddr, paddr)) {
    // Something goes wrong. The simulation process should
    // allocate this address.
    panic("Failed translating base %s(%p) + %p = %p to phy addr\n",
          base.c_str(), reinterpret_cast<void*>(base_vaddr),
          reinterpret_cast<void*>(offset), reinterpret_cast<void*>(simu_vaddr));
  }
  return paddr;
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
  DPRINTF(LLVMTraceCPU, "MapVirtualMem base %s to 0x%x\n", base.c_str(), vaddr);
  this->mapBaseToVAddr[base] = vaddr;
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
  schedule(this->tickEvent, curTick() + 1);
}
