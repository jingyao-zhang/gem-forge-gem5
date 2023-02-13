#include "exec_func_context.hh"

#include "debug/ExecFunc.hh"

#include "base/logging.hh"

namespace gem5 {

void ExecFuncContext::setRegOperand(const StaticInst *si, int idx,
                                    const void *val) {
  const RegId &regId = si->destRegIdx(idx);
  if (regId.is(InvalidRegClass)) {
    return;
  }

  const RegId reg = regId.flatten(*isa);

  auto &reg_file = regFiles[reg.classValue()];

  DPRINTF(ExecFunc, "Set Reg %s reg.index() %d.\n", regId, reg.index());

  reg_file->set(reg.index(), val);
}

} // namespace gem5