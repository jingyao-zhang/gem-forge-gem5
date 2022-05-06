#include "StreamFloatPlan.hh"

#include <sstream>

std::ostream &operator<<(std::ostream &os, const StreamFloatPlan &plan) {
  if (plan.changePoints.empty()) {
    os << "0 - " << MachineType_to_string(MachineType_NULL);
  } else {
    for (const auto &entry : plan.changePoints) {
      os << entry.second.startElementIdx << " - "
         << entry.second.floatMachineType << ", ";
    }
  }
  return os;
}

std::string to_string(const StreamFloatPlan &plan) {
  std::ostringstream ss;
  ss << plan;
  return ss.str();
}

void StreamFloatPlan::addFloatChangePoint(ElementIdx startElementIdx,
                                          MachineType floatMachineType) {
  assert(!this->finalized && "Finalized.");
  this->changePoints.emplace(
      std::piecewise_construct, std::forward_as_tuple(startElementIdx),
      std::forward_as_tuple(startElementIdx, floatMachineType));
}

void StreamFloatPlan::delayFloatUntil(ElementIdx firstFloatElementIdx) {
  assert(!this->finalized && "Finalized.");
  if (this->changePoints.empty()) {
    return;
  }
  // Erase any old change point, and remember the latest one before
  // FirstFloatElementIdx.
  FloatChangePoint latestPoint = this->changePoints.begin()->second;
  while (!this->changePoints.empty() &&
         this->changePoints.begin()->first < firstFloatElementIdx) {
    latestPoint = this->changePoints.begin()->second;
    this->changePoints.erase(this->changePoints.begin());
  }
  if (latestPoint.startElementIdx < firstFloatElementIdx) {
    this->addFloatChangePoint(firstFloatElementIdx,
                              latestPoint.floatMachineType);
  }
  // Add the first Core segments.
  this->addFloatChangePoint(0, MachineType::MachineType_NULL);
}

void StreamFloatPlan::finalize() {
  assert(!this->finalized && "Finalized.");
  assert(!this->changePoints.empty() && "Finalized without ChangePoints.");
  assert(this->changePoints.begin()->first == 0 &&
         "Finalized with NonZero FirstChangePoints.");
  this->finalizedFloatedToMem = this->isFloatedToMem();
  this->finalizedFirstFloatElementIdx = this->getFirstFloatElementIdx();
  this->finalized = true;
}

bool StreamFloatPlan::isFloatedToMem() const {
  if (this->finalized) {
    return this->finalizedFloatedToMem;
  }
  for (const auto &point : this->changePoints) {
    if (point.second.floatMachineType == MachineType::MachineType_Directory) {
      return true;
    }
  }
  return false;
}

bool StreamFloatPlan::isMixedFloat() const {
  if (this->changePoints.size() > 1) {
    return true;
  }
  return false;
}

StreamFloatPlan::ElementIdx StreamFloatPlan::getFirstFloatElementIdx() const {
  if (this->finalized) {
    return this->finalizedFirstFloatElementIdx;
  }
  assert(!this->changePoints.empty() &&
         "Query FirstFloatElementIdx on Empty FloatPlan.");
  for (const auto &point : this->changePoints) {
    if (point.second.floatMachineType != MachineType_NULL) {
      return point.first;
    }
  }
  assert(false && "Query FistFloatElementIdx on NotFloated FloatPlan.");
}

MachineType StreamFloatPlan::getMachineTypeAtElem(ElementIdx elementIdx) const {
  auto iter = this->changePoints.upper_bound(elementIdx);
  if (iter == this->changePoints.begin()) {
    panic("Somehow this element is not covered in FloatPlan.");
  }
  iter--;
  return iter->second.floatMachineType;
}