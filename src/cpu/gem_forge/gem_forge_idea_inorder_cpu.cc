#include "gem_forge_idea_inorder_cpu.hh"

#include "base/callback.hh"
#include "base/statistics.hh"
#include "debug/GemForgeIdeaInorderCPU.hh"

namespace gem5 {

GemForgeIdeaInorderCPU::GemForgeIdeaInorderCPU(
    int _cpuId, const BaseISA::RegClasses &regClasses, int _issueWidth,
    bool _modelFUTiming, bool _modelLDTiming)
    : cpuId(_cpuId), issueWidth(_issueWidth), modelFUTiming(_modelFUTiming),
      modelLDTiming(_modelLDTiming) {
  Stats::registerResetCallback([this]() -> void { this->resetCallback(); });

  // Init update mask.
  this->initUpdateMask(regClasses);
}

void GemForgeIdeaInorderCPU::initUpdateMask(
    const BaseISA::RegClasses &regClasses) {
  for (const auto &regClass : regClasses) {
    this->updateMask.emplace(std::piecewise_construct,
                             std::forward_as_tuple(regClass->type()),
                             std::forward_as_tuple(regClass->numRegs(), 0));
  }
}

void GemForgeIdeaInorderCPU::nextCycle() {
  this->cycles++;
  this->currentIssuedOps = 0;
  for (auto &mask : this->updateMask) {
    for (auto &lat : mask.second) {
      lat = std::max(0, lat - 1);
    }
  }
}

int GemForgeIdeaInorderCPU::getUpdateLat(const RegId &regId) {
  return this->updateMask.at(regId.classValue()).at(regId.index());
}

void GemForgeIdeaInorderCPU::updateLat(const RegId &regId, int opLat) {
  auto &lat = this->updateMask.at(regId.classValue()).at(regId.index());
  lat = std::max(opLat, lat);
}

void GemForgeIdeaInorderCPU::resetCallback() {
  for (auto &mask : this->updateMask) {
    std::fill(mask.second.begin(), mask.second.end(), 0);
  }
  this->committedOps = 0;
  this->cycles = 0;
  this->currentIssuedOps = 0;
}

void GemForgeIdeaInorderCPU::addOp(const GemForgeDynInstInfo &dynInfo) {
  auto op = dynInfo.staticInst;
  if (this->currentIssuedOps == this->issueWidth) {
    DPRINTF(GemForgeIdeaInorderCPU,
            "[%d]==== Advance as IssueLimit, issued %d.\n", this->cpuId,
            this->currentIssuedOps);
    this->nextCycle();
  }
  auto opClass = op->opClass();
  /**
   * ! In MinorCPU, it does not check source registers for No_OpClass.
   * Check for reg dependence.
   */
  if (opClass != OpClass::No_OpClass) {
    auto numSrcRegs = op->numSrcRegs();
    auto srcLat = getSrcLat(opClass);
    for (auto srcRegIdx = 0; srcRegIdx < numSrcRegs; ++srcRegIdx) {
      RegId reg = op->srcRegIdx(srcRegIdx);
      while (this->getUpdateLat(reg) > srcLat) {
        // We found a reg-dep.
        DPRINTF(
            GemForgeIdeaInorderCPU,
            "[%d]==== Advance as RAW RegDep lat %d, srcLat %d, issued %d.\n",
            this->cpuId, this->getUpdateLat(reg), srcLat,
            this->currentIssuedOps);
        this->nextCycle();
      }
    }
  }
  // We can issue this op.
  this->currentIssuedOps++;
  this->committedOps++;
  DPRINTF(GemForgeIdeaInorderCPU, "[%d] Issued %s.\n", this->cpuId,
          op->disassemble(dynInfo.pc.instAddr()));
  // Mark the dest regs. We ignore WAR and WAW deps.
  // ! Also ignore No_OpClass. Consistent with MinorCPU.
  if (opClass != OpClass::No_OpClass) {
    auto numDestRegs = op->numDestRegs();
    for (auto destRegIdx = 0; destRegIdx < numDestRegs; ++destRegIdx) {
      RegId reg = op->destRegIdx(destRegIdx);
      auto opLat = getDestLat(opClass);
      this->updateLat(reg, opLat);
    }
  }
}

int GemForgeIdeaInorderCPU::getDestLat(OpClass opClass) const {
  int opLat = 1;
  if (!this->modelFUTiming) {
    return opLat;
  }
  // Keep in consistent with MinorCPU.py
  switch (opClass) {
  case OpClass::IntAlu: {
    opLat = 3;
    break;
  }
  case OpClass::FloatMemRead:
  case OpClass::MemRead: {
    if (!this->modelLDTiming) {
      opLat = 1;
    } else {
      opLat = 3;
    }
    break;
  }
  case OpClass::FloatAdd:
  case OpClass::FloatCmp:
  case OpClass::FloatCvt: {
    opLat = 2;
    break;
  }
  case OpClass::FloatMult:
  case OpClass::FloatMultAcc:
  case OpClass::FloatMisc: {
    opLat = 4;
    break;
  }
  case OpClass::FloatDiv: {
    opLat = 12;
    break;
  }
  case OpClass::FloatSqrt: {
    opLat = 24;
    break;
  }
  case OpClass::IntMult: {
    opLat = 3;
    break;
  }
  case OpClass::IntDiv: {
    if (THE_ISA == X86_ISA) {
      opLat = 1;
    } else {
      opLat = 9;
    }
    break;
  }
  default: {
    opLat = 1;
  }
  }
  return opLat;
}

int GemForgeIdeaInorderCPU::getSrcLat(OpClass opClass) const {
  int opLat = 0;
  if (!this->modelFUTiming) {
    return opLat;
  }
  // Keep in consistent with MinorCPU.py
  switch (opClass) {
  case OpClass::IntAlu: {
    opLat = 2;
    break;
  }
  case OpClass::FloatMemRead:
  case OpClass::FloatMemWrite:
  case OpClass::MemRead:
  case OpClass::MemWrite: {
    opLat = 1;
    break;
  }
  case OpClass::FloatAdd:
  case OpClass::FloatCmp:
  case OpClass::FloatCvt: {
    opLat = 0;
    break;
  }
  case OpClass::FloatMult:
  case OpClass::FloatMultAcc:
  case OpClass::FloatMisc: {
    opLat = 2;
    break;
  }
  case OpClass::FloatDiv: {
    opLat = 0;
    break;
  }
  case OpClass::FloatSqrt: {
    opLat = 0;
    break;
  }
  case OpClass::IntMult: {
    opLat = 0;
    break;
  }
  case OpClass::IntDiv: {
    opLat = 0;
    break;
  }
  default: {
    opLat = 0;
  }
  }
  return opLat;
}
} // namespace gem5
