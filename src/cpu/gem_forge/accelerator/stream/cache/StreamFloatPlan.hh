#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_CACHE_STREAM_FLOAT_PLAN_H__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_CACHE_STREAM_FLOAT_PLAN_H__

/**
 * This represents the float plan with a vector of changing levels:
 * (ElementIdx, Level).
 * This means that starting from ElementIdx, the stream is at Level.
 * We use MachineType_NULL to represent Core (not floated).
 */

#include "mem/ruby/protocol/MachineType.hh"

#include <map>

struct StreamFloatPlan {

public:
  using ElementIdx = uint64_t;

  struct FloatChangePoint {
    ElementIdx startElementIdx;
    MachineType floatMachineType;
    FloatChangePoint(ElementIdx _startElementIdx, MachineType _floatMachineType)
        : startElementIdx(_startElementIdx),
          floatMachineType(_floatMachineType) {}
  };

  std::map<ElementIdx, FloatChangePoint> changePoints;

  void clear() { this->changePoints.clear(); }
  void addFloatChangePoint(ElementIdx startElementIdx,
                           MachineType floatMachineType);

  /**
   * Delay the FloatPlan until FirstFloatElementIdx.
   * This is AdHoc implementation for MidwayFloating, should be improved.
   */
  void delayFloatUntil(ElementIdx firstFloatElementIdx);

  /**
   * Finalize the plan.
   * Basically cache some frequently used information.
   */
  void finalize();

  /**
   * Some query functions.
   */
  bool isFloatedToMem() const;
  ElementIdx getFirstFloatElementIdx() const;
  MachineType getMachineTypeAtElem(ElementIdx elementIdx) const;

private:
  bool finalized = false;
  bool finalizedFloatedToMem = false;
  ElementIdx finalizedFirstFloatElementIdx = 0;
};

std::ostream &operator<<(std::ostream &os, const StreamFloatPlan &plan);

std::string to_string(const StreamFloatPlan &plan);

#endif