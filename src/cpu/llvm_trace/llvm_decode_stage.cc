#include "cpu/llvm_trace/llvm_decode_stage.hh"
#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "debug/LLVMTraceCPU.hh"

using InstStatus = LLVMTraceCPU::InstStatus;

LLVMDecodeStage::LLVMDecodeStage(LLVMTraceCPUParams* params, LLVMTraceCPU* _cpu)
    : cpu(_cpu),
      decodeWidth(params->decodeWidth),
      decodeQueueSize(params->decodeQueueSize),
      fromFetchDelay(params->fetchToDecodeDelay),
      toRenameDelay(params->decodeToRenameDelay) {}

void LLVMDecodeStage::setToRename(TimeBuffer<FetchStruct>* toRenameBuffer) {
  this->toRename = toRenameBuffer->getWire(0);
}

void LLVMDecodeStage::setFromFetch(TimeBuffer<FetchStruct>* fromFetchBuffer) {
  this->fromFetch = fromFetchBuffer->getWire(-this->fromFetchDelay);
}

void LLVMDecodeStage::setSignal(TimeBuffer<LLVMStageSignal>* signalBuffer,
                                int pos) {
  this->signal = signalBuffer->getWire(pos);
}

void LLVMDecodeStage::regStats() {}

void LLVMDecodeStage::tick() {
  // Get fetched inst from fetch.
  for (auto iter = this->fromFetch->begin(), end = this->fromFetch->end();
       iter != end; iter++) {
    this->decodeQueue.push(*iter);
  }

  // Only decode if we haven't reach decode width and we have more inst to
  // decode.
  if (!this->signal->stall) {
    unsigned decodedInsts = 0;
    while (decodedInsts < this->decodeWidth && !this->decodeQueue.empty()) {
      auto instId = this->decodeQueue.front();
      this->decodeQueue.pop();
      DPRINTF(LLVMTraceCPU, "Decode inst %d\n", instId);
      cpu->inflyInsts[instId] = InstStatus::DECODED;
      this->toRename->push_back(instId);
      decodedInsts++;
    }
  }

  // Raise stall if our decodeQueue reaches limits.
  this->signal->stall = this->decodeQueue.size() >= this->decodeQueueSize;
}
