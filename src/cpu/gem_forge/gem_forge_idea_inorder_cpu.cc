#include "gem_forge_idea_inorder_cpu.hh"

#include "base/callback.hh"
#include "base/statistics.hh"
#include "debug/GemForgeIdeaInorderCPU.hh"

namespace {
/**
 * A helper function to map RegId to our internal index.
 */
bool findIndex(const RegId &reg, int &scoreboard_index) {
  bool ret = false;

  if (reg.isZeroReg()) {
    /* Don't bother with the zero register */
    ret = false;
  } else {
    switch (reg.classValue()) {
    case IntRegClass:
      scoreboard_index = reg.index();
      ret = true;
      break;
    case FloatRegClass:
      scoreboard_index = TheISA::NumIntRegs + TheISA::NumCCRegs + reg.index();
      ret = true;
      break;
    case VecRegClass:
      scoreboard_index = TheISA::NumIntRegs + TheISA::NumCCRegs +
                         TheISA::NumFloatRegs + reg.index();
      ret = true;
      break;
    case VecElemClass:
      scoreboard_index = TheISA::NumIntRegs + TheISA::NumCCRegs +
                         TheISA::NumFloatRegs + reg.flatIndex();
      ret = true;
      break;
    case VecPredRegClass:
      scoreboard_index = TheISA::NumIntRegs + TheISA::NumCCRegs +
                         TheISA::NumFloatRegs + TheISA::NumVecRegs +
                         reg.index();
      ret = true;
      break;
    case CCRegClass:
      scoreboard_index = TheISA::NumIntRegs + reg.index();
      ret = true;
      break;
    case MiscRegClass:
      /* Don't bother with Misc registers */
      ret = false;
      break;
    default:
      panic("Unknown register class: %d", static_cast<int>(reg.classValue()));
    }
  }

  return ret;
}

} // namespace

GemForgeIdeaInorderCPU::GemForgeIdeaInorderCPU(int _cpuId, int _issueWidth,
                                               bool _modelFUTiming,
                                               bool _modelLDTiming)
    : cpuId(_cpuId), issueWidth(_issueWidth), modelFUTiming(_modelFUTiming),
      modelLDTiming(_modelLDTiming), updateMask(numRegs, 0) {
  Stats::registerResetCallback(
      new MakeCallback<GemForgeIdeaInorderCPU,
                       &GemForgeIdeaInorderCPU::resetCallback>(this, true));
}

void GemForgeIdeaInorderCPU::nextCycle() {
  this->cycles++;
  this->currentIssuedOps = 0;
  for (auto &lat : this->updateMask) {
    lat = std::max(0, lat - 1);
  }
}

void GemForgeIdeaInorderCPU::resetCallback() {
  std::fill(this->updateMask.begin(), this->updateMask.end(), 0);
  this->committedOps = 0;
  this->cycles = 0;
  this->currentIssuedOps = 0;
}

void GemForgeIdeaInorderCPU::addOp(const GemForgeDynInstInfo &dynInfo) {
  auto op = dynInfo.staticInst;
  auto tc = dynInfo.tc;
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
      RegId reg = tc->flattenRegId(op->srcRegIdx(srcRegIdx));
      int index;
      if (findIndex(reg, index)) {
        while (this->updateMask.at(index) > srcLat) {
          // We found a reg-dep.
          DPRINTF(
              GemForgeIdeaInorderCPU,
              "[%d]==== Advance as RAW RegDep lat %d, srcLat %d, issued %d.\n",
              this->cpuId, this->updateMask.at(index), srcLat,
              this->currentIssuedOps);
          this->nextCycle();
        }
      }
    }
  }
  // We can issue this op.
  this->currentIssuedOps++;
  this->committedOps++;
  DPRINTF(GemForgeIdeaInorderCPU, "[%d] Issued %s.\n", this->cpuId,
          op->disassemble(dynInfo.pc.pc()));
  // Mark the dest regs. We ignore WAR and WAW deps.
  // ! Also ignore No_OpClass. Consistent with MinorCPU.
  if (opClass != OpClass::No_OpClass) {
    auto numDestRegs = op->numDestRegs();
    for (auto destRegIdx = 0; destRegIdx < numDestRegs; ++destRegIdx) {
      RegId reg = tc->flattenRegId(op->destRegIdx(destRegIdx));
      int index;
      if (findIndex(reg, index)) {
        auto &lat = this->updateMask.at(index);
        auto opLat = getDestLat(opClass);
        lat = std::max(opLat, lat);
      }
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
  default: { opLat = 1; }
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
  default: { opLat = 0; }
  }
  return opLat;
}
