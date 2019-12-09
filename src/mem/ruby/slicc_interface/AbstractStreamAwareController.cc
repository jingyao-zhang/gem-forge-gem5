#include "AbstractStreamAwareController.hh"

#include "RubySlicc_ComponentMapping.hh"

AbstractStreamAwareController::AbstractStreamAwareController(const Params *p)
    : AbstractController(p), llcSelectLowBit(p->llc_select_low_bit),
      llcSelectNumBits(p->llc_select_num_bits),
      enableStreamFloat(p->enable_stream_float),
      enableStreamSubline(p->enable_stream_subline),
      mlcStreamBufferInitNumEntries(p->mlc_stream_buffer_init_num_entries) {}

MachineID
AbstractStreamAwareController::mapAddressToLLC(Addr addr,
                                               MachineType mtype) const {
  // Ideally we should check mtype to be LLC or directory, etc.
  // But here I ignore it.
  return mapAddressToRange(addr, mtype, this->llcSelectLowBit,
                           this->llcSelectNumBits, 0 /* cluster_id. */
  );
}

  Addr AbstractStreamAwareController::getAddressToOurLLC() const {
    // Make it simple.
    return this->getMachineID().num << this->llcSelectLowBit;
  }
