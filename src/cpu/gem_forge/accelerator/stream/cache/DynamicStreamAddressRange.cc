#include "DynamicStreamAddressRange.hh"

DynamicStreamAddressRange::DynamicStreamAddressRange(
    const DynamicStreamElementRangeId &_elementRange,
    const AddressRange &_vaddrRange, const AddressRange &_paddrRange)
    : elementRange(_elementRange), vaddrRange(_vaddrRange),
      paddrRange(_paddrRange) {}

void DynamicStreamAddressRange::addRange(DynamicStreamAddressRangePtr &range) {
  // Use the first one as my element range.
  if (!this->isValid()) {
    this->elementRange = range->elementRange;
  }

  // If this is the first union, we also allocate a subrange for myself.
  if (this->subRanges.empty()) {
    auto self = std::make_shared<DynamicStreamAddressRange>(
        this->elementRange, this->vaddrRange, this->paddrRange);
    this->subRanges.push_back(self);
  }

  this->vaddrRange.add(range->vaddrRange);
  this->paddrRange.add(range->paddrRange);
  if (range->isUnion()) {
    for (auto &r : range->subRanges) {
      this->subRanges.push_back(r);
    }
  } else {
    this->subRanges.push_back(range);
  }
}

std::ostream &operator<<(std::ostream &os, const AddressRange &range) {
  os << std::hex << range.lhs << " - " << range.rhs << std::dec;
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const DynamicStreamAddressRange &range) {
  if (range.isUnion()) {
    os << "\n  Union Range Vaddr " << range.vaddrRange << " Paddr "
       << range.paddrRange << "\n";
    for (const auto &r : range.subRanges) {
      os << "    " << *r << "\n";
    }
    os << "  Union Range Done\n";
    return os;
  } else {
    os << range.elementRange << " Vaddr " << range.vaddrRange << " Paddr "
       << range.paddrRange;
    return os;
  }
}