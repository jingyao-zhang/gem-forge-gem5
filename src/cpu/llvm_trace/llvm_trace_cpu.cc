#include <fstream>

#include "debug/LLVMTraceCPU.hh"
#include "llvm_trace_cpu.hh"

LLVMTraceCPU::LLVMTraceCPU(LLVMTraceCPUParams* params)
    : BaseCPU(params),
      pageTable(params->name + ".page_table", 0),
      instPort(params->name + ".inst_port", this),
      dataPort(params->name + ".data_port", this),
      traceFile(params->traceFile),
      currentInstId(0),
      scheduleNextEvent(*this) {
  DPRINTF(LLVMTraceCPU, "LLVMTraceCPU constructed\n");
  readTraceFile();
  // Schedule the first event.
  schedule(this->scheduleNextEvent, curTick() + 1);
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
    Addr vaddr = 0x0;
    if (fields[0] == "s") {
      type = DynamicInst::Type::STORE;
      vaddr = std::stoul(fields[2], nullptr, 16);
    } else if (fields[0] == "l") {
      type = DynamicInst::Type::LOAD;
      vaddr = std::stoul(fields[2], nullptr, 16);
    }
    this->dynamicInsts.emplace_back(type, delay, vaddr);
    DPRINTF(LLVMTraceCPU,
            "Parsed #%u dynamic inst with type %u, delay %u, vaddr 0x%x\n",
            this->dynamicInsts.size(), type, delay, vaddr);
  }
  DPRINTF(LLVMTraceCPU, "Parsed total number of inst: %u\n",
          this->dynamicInsts.size());
}

void LLVMTraceCPU::processScheduleNextEvent() {
  DPRINTF(LLVMTraceCPU, "Processing scheduleNextEvent!\n");
  if (this->currentInstId == this->dynamicInsts.size()) {
    DPRINTF(LLVMTraceCPU, "We have no inst left to be scheduled.\n");
    return;
  }
  const DynamicInst& inst = this->dynamicInsts[this->currentInstId];
  DynamicInstId instId = this->currentInstId;
  this->currentInstId++;

  switch (inst.type) {
    case DynamicInst::Type::COMPUTE: {
      // Just schedule for next after delay.
      schedule(this->scheduleNextEvent, curTick() + inst.computeDelay);
      break;
    }
    case DynamicInst::Type::LOAD:
    case DynamicInst::Type::STORE: {
      Addr paddr = this->translateAndAllocatePhysMem(inst.vaddr);
      RequestPtr req =
          new Request(paddr, 4, 0, this->_dataMasterId, instId, ContextID(0));
      PacketPtr pkt;
      uint8_t* pkt_data = new uint8_t[req->getSize()];
      if (inst.type == DynamicInst::Type::LOAD) {
        pkt = Packet::createRead(req);
      } else {
        pkt = Packet::createWrite(req);
        // Store some dummy data.
        memset(pkt_data, 0xB, req->getSize());
      }
      pkt->dataDynamic(pkt_data);
      this->inflyInstIds.insert(instId);
      this->dataPort.sendReq(pkt);
      break;
    }
    default: { panic("Unknown dynamic instruction type %u\n", inst.type); }
  }
}

bool LLVMTraceCPU::handleTimingResp(PacketPtr pkt) {
  // Receive the response from port.
  DynamicInstId instId = pkt->req->getReqInstSeqNum();
  DynamicInst& inst = this->dynamicInsts[instId];
  schedule(this->scheduleNextEvent, curTick() + inst.computeDelay);
  DPRINTF(LLVMTraceCPU,
          "Get response for inst %u, schedule next inst after %u\n", instId,
          inst.computeDelay);
  this->inflyInstIds.erase(instId);
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

void LLVMTraceCPU::CPUPort::sendReq(PacketPtr pkt) {
  // Panic if need retry.
  bool success = MasterPort::sendTimingReq(pkt);
  if (!success) {
    panic("Failed sending request!");
  }
}
