#ifndef __GEM_FORGE_IDEA_INORDER_CPU_HH__
#define __GEM_FORGE_IDEA_INORDER_CPU_HH__

/**
 * An ideal inorder core that can be used to find the maximum ipc for
 * an inorder core.
 * It only considers register dependence and assumes all instructions
 * has one cycle latency.
 */

#include "gem_forge_dyn_inst_info.hh"

class GemForgeIdeaInorderCPU {
public:
  GemForgeIdeaInorderCPU(int _cpuId, int _issueWidth, bool _modelFUTiming,
                         bool _modelLDTiming);

  void addOp(const GemForgeDynInstInfo &dynInfo);
  float getOPC() const {
    return this->cycles == 0 ? 0.0f
                             : static_cast<float>(this->committedOps) /
                                   static_cast<float>(this->cycles);
  }
  float getCycles() const { return static_cast<float>(this->cycles); }

private:
  const int cpuId;
  const int issueWidth;
  /**
   * Whether the timing of FU is considered.
   * When false, all FU takes 1 cycle latency.
   * This is the most ideal case.
   */
  const bool modelFUTiming;
  /**
   * Whether the timing of a load accessing cache is considered.
   * When false, a load takes only 1 cycle, not 3 cycle.
   */
  const bool modelLDTiming;
  uint64_t committedOps = 0;
  uint64_t cycles = 0;

  /**
   * Intermediate states for current cycle.
   */
  int currentIssuedOps = 0;

  static constexpr int numRegs =
      TheISA::NumIntRegs + TheISA::NumCCRegs + TheISA::NumFloatRegs +
      (TheISA::NumVecRegs * TheISA::NumVecElemPerVecReg) +
      TheISA::NumVecPredRegs;
  /**
   * A simple scoreboard.
   * Avoid using specialized std::vector<bool> as we don't care.
   */
  std::vector<int> updateMask;

  void nextCycle();
  void resetCallback();
  int getSrcLat(OpClass opClass) const;
  int getDestLat(OpClass opClass) const;
};

#endif