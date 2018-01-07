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
      currentInstId(0),
      process(nullptr),
      thread_context(nullptr),
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
 * Split a string like a,b,c, into [a, b, c].
 */
std::vector<std::string> splitByComma(const std::string& source) {
  std::vector<std::string> ret;
  for (size_t idx = 0, prev = 0; idx < source.size(); ++idx) {
    if (source[idx] == ',') {
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
    auto fields = splitByComma(line);
    // Default is compute type.
    DynamicInst::Type type = DynamicInst::Type::COMPUTE;
    Tick delay = std::stoul(fields[1]);
    // Fields for LOAD/STORE.
    int size = -1;
    std::string base = "";
    Addr offset = 0x0;
    Addr trace_vaddr = 0x0;
    // Fields for STORE.
    void* value = nullptr;

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
          value = new double;
          *((double*)value) = stod(fields[7]);
          break;
        }
        default:
          fatal("Unsupported type id %d\n", typeId);
      }
    } else if (fields[0] == "l") {
      type = DynamicInst::Type::LOAD;
      base = fields[2];
      offset = stoull(fields[3]);
      trace_vaddr = stoull(fields[4]);
      size = stoi(fields[5]);
    }
    this->dynamicInsts.emplace_back(type, delay, size, base, offset,
                                    trace_vaddr, value);
    DPRINTF(LLVMTraceCPU,
            "Parsed #%u dynamic inst with type %u, delay %u, size %d, base %s, "
            "offset 0x%x, trace_vaddr 0x%x\n",
            this->dynamicInsts.size(), type, delay, size, base, offset,
            trace_vaddr);
  }
  DPRINTF(LLVMTraceCPU, "Parsed total number of inst: %u\n",
          this->dynamicInsts.size());
}

void LLVMTraceCPU::fetch() {
  while (this->currentInstId < this->dynamicInsts.size() &&
         this->fetchQueue.size() < this->MAX_FETCH_QUEUE_SIZE) {
    DPRINTF(LLVMTraceCPU, "Fetch inst %d into fetchQueue\n",
            this->currentInstId);
    this->inflyInsts[currentInstId] = InstStatus::FETCHED;
    this->fetchQueue.push(this->currentInstId++);
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

      const DynamicInst& inst = this->dynamicInsts[instId];

      switch (inst.type) {
        case DynamicInst::Type::COMPUTE: {
          // Just do nothing.
          break;
        }
        case DynamicInst::Type::LOAD:
        case DynamicInst::Type::STORE: {
          Addr paddr;
          if (this->driver == nullptr) {
            // When in stand alone mode, use the trace space address
            // directly as the virtual address.
            paddr = this->translateAndAllocatePhysMem(inst.trace_vaddr);
          } else {
            // When we have a driver, we have to translate trace space
            // address into simulation space and then use the process
            // page table to get physical address.
            paddr = this->translateTraceToPhysMem(inst.base, inst.offset);
          }
          RequestPtr req =
              new Request(paddr, inst.size, 0, this->_dataMasterId, instId,
                          this->thread_context->contextId());
          PacketPtr pkt;
          uint8_t* pkt_data = new uint8_t[req->getSize()];
          if (inst.type == DynamicInst::Type::LOAD) {
            pkt = Packet::createRead(req);
          } else {
            pkt = Packet::createWrite(req);
            // Copy the value to store.
            memcpy(pkt_data, inst.value, req->getSize());
          }
          pkt->dataDynamic(pkt_data);
          DPRINTF(LLVMTraceCPU, "Send request for inst %d\n", instId);
          this->dataPort.sendReq(pkt);
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
      DynamicInst& inst = this->dynamicInsts[instId];
      if (inst.type != DynamicInst::Type::COMPUTE) {
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
      DPRINTF(LLVMTraceCPU, "Inst %u committed\n", instId);
      // Mark the rob entry as free again.
      this->rob[i].second = false;
      // Erase it from the inflyInsts.
      this->inflyInsts.erase(instId);
    }
  }
}

void LLVMTraceCPU::tick() {
  if (curTick() % 1000000 == 0) {
    DPRINTF(LLVMTraceCPU, "Tick()\n");
  }

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
    // Write 1 to the finish_tag.
    RequestPtr req =
        new Request(this->finish_tag_paddr, 1, 0, this->_dataMasterId,
                    this->currentInstId, ContextID(0));
    PacketPtr pkt;
    uint8_t* pkt_data = new uint8_t[1];
    pkt_data[0] = 1;
    pkt = Packet::createWrite(req);
    pkt->dataDynamic(pkt_data);
    this->dataPort.sendReq(pkt);
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
    DPRINTF(LLVMTraceCPU, "Get response for inst %u, mark it as finished\n",
            instId);
    panic_if(this->inflyInsts.find(instId) == this->inflyInsts.end(),
             "inst %u should be in inflyInsts\n", instId);
    panic_if(this->inflyInsts[instId] != InstStatus::ISSUED,
             "inst %u should be issued to be finished\n", instId);
    this->inflyInsts[instId] = InstStatus::FINISHED;
  } else {
    DPRINTF(LLVMTraceCPU, "Finish writing finish_tag\n");
  }
  // Release the memory.
  delete pkt->req;
  delete pkt;

  // After receive some response, we retry blocked packet.
  this->dataPort.recvReqRetry();
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
  if (!this->blockedPacketPtrs.empty()) {
    DPRINTF(LLVMTraceCPU, "Blocked packet ptr %p\n", pkt);
    this->blockedPacketPtrs.push(pkt);
    return;
  }
  // Push to blocked ptrs if need retry.
  bool success = MasterPort::sendTimingReq(pkt);
  if (!success) {
    DPRINTF(LLVMTraceCPU, "Blocked packet ptr %p\n", pkt);
    this->blockedPacketPtrs.push(pkt);
  }
}

void LLVMTraceCPU::CPUPort::recvReqRetry() {
  std::lock_guard<std::mutex> guard(this->blockedPacketPtrsMutex);
  if (!this->blockedPacketPtrs.empty()) {
    // If we have blocked packet, send again.
    PacketPtr pkt = this->blockedPacketPtrs.front();
    bool success = MasterPort::sendTimingReq(pkt);
    DPRINTF(LLVMTraceCPU, "Retry blocked packet ptr %p\n", pkt);
    if (success) {
      DPRINTF(LLVMTraceCPU, "Retry blocked packet ptr %p: Succeed\n", pkt);
      this->blockedPacketPtrs.pop();
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

  if (!this->process->pTable->translate(finish_tag_vaddr,
                                        this->finish_tag_paddr)) {
    panic("Failed translating finish_tag_vaddr %p to paddr\n",
          reinterpret_cast<void*>(finish_tag_vaddr));
  }

  // Schedule the next event.
  schedule(this->tickEvent, curTick() + 1);
}
