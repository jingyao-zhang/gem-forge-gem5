#include "gem_forge_isa_handler.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"

#define GEM_FORGE_INST_GET_CPU() (xc.tcBase()->getCpuPtr())
#define GEM_FORGE_INST_GET_ACCEL_MANAGER()                                     \
  (GEM_FORGE_INST_GET_CPU()->getAccelManager())
#define GEM_FORGE_INST_GET_STREAM_ENGINE()                                     \
  (GEM_FORGE_INST_GET_ACCEL_MANAGER()->getStreamEngine())

namespace RiscvISA {
bool GemForgeISAHandler::canDispatch(const GemForgeDynInstInfo &dynInfo,
                                     ExecContext &xc) {
  auto &staticInstInfo = this->getStaticInstInfo(xc.pcState(), dynInfo);
  if (staticInstInfo.op == GemForgeStaticInstOpE::STREAM_CONFIG) {
    auto se = GEM_FORGE_INST_GET_STREAM_ENGINE();
    auto rs1 = xc.readIntRegOperand(dynInfo.staticInst, 0);
    se->canStreamConfig(dynInfo.seqNum, rs1);
    return true;
  }
  return true;
}

GemForgeISAHandler::GemForgeStaticInstInfo &
GemForgeISAHandler::getStaticInstInfo(const TheISA::PCState &pcState,
                                      const GemForgeDynInstInfo &dynInfo) {
  auto pc = pcState.pc();
  auto emplaceRet = this->cachedStaticInstInfo.emplace(
      std::piecewise_construct, std::forward_as_tuple(pc),
      std::forward_as_tuple());
  if (emplaceRet.second) {
    // Newly created. Do basic analysis.
    // I am very surprised that Gem5 does not provide an easy way for me to get
    // the opcode of the inst. So far this is so fragile.
    // TODO: Improve this.
    auto instName = dynInfo.staticInst->getName();
    auto &staticInstInfo = emplaceRet.first->second;

    if (instName == "ssp_stream_config") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_CONFIG;
    }
  }
  return emplaceRet.first->second;
}

} // namespace RiscvISA