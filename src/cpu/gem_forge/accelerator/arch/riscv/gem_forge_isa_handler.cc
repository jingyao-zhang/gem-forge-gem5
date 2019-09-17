#include "gem_forge_isa_handler.hh"

#include "cpu/base.hh"
#include "cpu/exec_context.hh"

namespace RiscvISA {
bool GemForgeISAHandler::canDispatch(const GemForgeDynInstInfo &dynInfo,
                                     ExecContext &xc) {
  auto &staticInstInfo = this->getStaticInstInfo(xc.pcState(), dynInfo);
  switch (staticInstInfo.op) {
  case GemForgeStaticInstOpE::STREAM_CONFIG: {
    return se.canDispatchStreamConfig(dynInfo, xc);
  }
  case GemForgeStaticInstOpE::STREAM_STEP: {
    return se.canDispatchStreamStep(dynInfo, xc);
  }
  default: { return true; }
  }
}

void GemForgeISAHandler::dispatch(const GemForgeDynInstInfo &dynInfo,
                                  ExecContext &xc) {
  auto &staticInstInfo = this->getStaticInstInfo(xc.pcState(), dynInfo);
  switch (staticInstInfo.op) {
  case GemForgeStaticInstOpE::STREAM_CONFIG: {
    se.dispatchStreamConfig(dynInfo, xc);
    break;
  }
  case GemForgeStaticInstOpE::STREAM_END: {
    se.dispatchStreamEnd(dynInfo, xc);
    break;
  }
  case GemForgeStaticInstOpE::STREAM_STEP: {
    se.dispatchStreamStep(dynInfo, xc);
    break;
  }
  default: { break; }
  }
}

void GemForgeISAHandler::execute(const GemForgeDynInstInfo &dynInfo,
                                 ExecContext &xc) {
  auto &staticInstInfo = this->getStaticInstInfo(xc.pcState(), dynInfo);
  switch (staticInstInfo.op) {
  case GemForgeStaticInstOpE::STREAM_CONFIG: {
    se.executeStreamConfig(dynInfo, xc);
    break;
  }
  default: { break; }
  }
}

void GemForgeISAHandler::commit(const GemForgeDynInstInfo &dynInfo,
                                ExecContext &xc) {
  auto &staticInstInfo = this->getStaticInstInfo(xc.pcState(), dynInfo);
  switch (staticInstInfo.op) {
  case GemForgeStaticInstOpE::STREAM_CONFIG: {
    se.commitStreamConfig(dynInfo, xc);
    break;
  }
  case GemForgeStaticInstOpE::STREAM_END: {
    se.commitStreamEnd(dynInfo, xc);
    break;
  }
  case GemForgeStaticInstOpE::STREAM_STEP: {
    se.commitStreamStep(dynInfo, xc);
    break;
  }
  default: { break; }
  }
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
    } else if (instName == "ssp_stream_end") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_END;
    } else if (instName == "ssp_stream_step") {
      staticInstInfo.op = GemForgeStaticInstOpE::STREAM_STEP;
    }
  }
  return emplaceRet.first->second;
}

} // namespace RiscvISA