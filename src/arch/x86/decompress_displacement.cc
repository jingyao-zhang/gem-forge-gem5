#include "arch/x86/decoder.hh"

#include "arch/x86/regs/misc.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/Decoder.hh"

#include <unordered_set>

namespace X86ISA {

void Decoder::processCompressedDisplacement() {
  /**
   * ! Compressed Displacement.
   * EVEX instructions with one byte displacement always
   * uses compressed displacement. The displacement should
   * be multiplied by a factor N.
   *
   * It is quite complicated, so far we only do this in adhoc way.
   *
   */
  if (!emi.evex.evex_present) {
    return;
  }
  if (emi.dispSize != 1) {
    return;
  }
  assert(emi.evex.l_prime == 1 && "Can only handle EVEX.512.");
  // Convert to signed int64_t.
  int64_t displacment = emi.displacement;
  int64_t N = 1;
  if (emi.opcode.type == TwoByteOpcode) {
    if (emi.legacy.decodeVal == 0x1) {
      switch (emi.opcode.op) {
      case 0xD4: // vpaddq
      case 0xFE: // vpaddd
      case 0x6F: // vmovdqa(load)
      case 0x7F: // vmovdqa(store)
        N = 64;
        break;
      }
    } else if (emi.legacy.decodeVal == 0x4) {
      switch (emi.opcode.op) {
      case 0x6F: // vmovdqu(load)
      case 0x7F: // vmovdqu(store)
        N = 64;
        break;
      }
    } else {
      switch (emi.opcode.op) {
      case 0x10: // vmovups(load)
      case 0x11: // vmovups(store)
      case 0x28: // vmovaps(load)
      case 0x29: // vmovaps(store)
        N = 64;
        break;
      }
    }
  } else if (emi.opcode.type == ThreeByte0F38Opcode) {
    switch (emi.opcode.op) {
    case 0x39: // vpminsd, vpminsq
      N = 64;
      break;
    }
  }
  if (N == 1) {
    panic("Don't know how to expand displacement for %#x %s.\n", this->origPC,
          emi);
  }
  DPRINTF(Decoder, "Expand displacement pc %#x op %x %lld x %lld = %lld.\n",
          this->origPC, emi.opcode.op, displacment, N, displacment * N);
  hack("Expand displacement pc %#x op %x %lld x %lld = %lld.\n", this->origPC,
       emi.opcode.op, displacment, N, displacment * N);
  emi.displacement = displacment * N;
}

namespace {
std::unordered_set<int> VEX_TWO_BYTE_OP_IMPL = {
    0x10, // vmovups(load)
    0x11, // vmovups(store)
    0x28, // vmovaps(load)
    0x29, // vmovups(store)
    0x57, // vxorps
};
std::unordered_set<int> VEX_TWO_BYTE_OP_0x66_IMPL = {
    0x6F, // vmovdqa(load)
    0x7E, // vmovq(store)
    0x7F, // vmovdqa(store)
    0x70, // vpshufd
    0xD4, // vpaddq
    0xEF, // vpxor
    0xFE, // vpaddd
};
std::unordered_set<int> VEX_TWO_BYTE_OP_0xF3_IMPL = {
    0x6F, // vmovdqu(load)
    0x7F, // vmovdqu(store)
};
std::unordered_set<int> VEX_0F3A_OP_IMPL = {
    0x39, // vextract
};
std::unordered_set<int> EVEX_TWO_BYTE_OP_IMPL = {
    0x10, // vmovups(load)
    0x11, // vmovups(store)
    0x28, // vmovaps(load)
    0x29, // vmovaps(store)
};
std::unordered_set<int> EVEX_TWO_BYTE_OP_0x66_IMPL = {
    0x6F, // vmovdqa(load)
    0xD4, // vpaddq
    0xFE, // vpaddd
};
std::unordered_set<int> EVEX_TWO_BYTE_OP_0xF3_IMPL = {
    0x6F, // vmovdqu(load)
    0x7F, // vmovdqu(store)
};
std::unordered_set<int> EVEX_0F3A_OP_IMPL = {
    0x3B, // vextract
};
std::unordered_set<int> EVEX_0F38_OP_IMPL = {
    0x59, // vpbroadcastq
    0x39, // vpminsd, vpminsq
};
} // namespace

// Remove this once you feel confident that instructions are supported.
void Decoder::sanityCheckSIMD() {
  if (emi.evex.evex_present) {
    bool implemented = false;
    // Sanity check that we have implemented the evex instruction.
    if (emi.opcode.type == ThreeByte0F38Opcode) {
      implemented = EVEX_0F38_OP_IMPL.count(emi.opcode.op);
    } else if (emi.opcode.type == ThreeByte0F3AOpcode) {
      implemented = EVEX_0F3A_OP_IMPL.count(emi.opcode.op);
    } else if (emi.opcode.type == TwoByteOpcode) {
      switch (emi.legacy.decodeVal) {
        case 0x0:
          implemented = EVEX_TWO_BYTE_OP_IMPL.count(emi.opcode.op);
          break;
        case 0x1:
          implemented = EVEX_TWO_BYTE_OP_0x66_IMPL.count(emi.opcode.op);
          break;
        case 0x4:
          implemented = EVEX_TWO_BYTE_OP_0xF3_IMPL.count(emi.opcode.op);
          break;
      }
      if (emi.legacy.decodeVal == 0x1 && emi.opcode.op == 0xD4) {
        hack("%#x vpaddq with v_extend %d v %d v_prime %d.\n",
          origPC, emi.evex.v_extend, emi.evex.v, emi.evex.v_prime);
      }
    }
    if (!implemented) {
      panic("Unimplemented evex inst %#x:\n%s.\n", basePC, emi);
    }
  }

  if (emi.evex.vex_present) {
    // 256bit simd.
    bool implemented = false;
    if (emi.opcode.type == ThreeByte0F3AOpcode) {
      implemented = VEX_0F3A_OP_IMPL.count(emi.opcode.op);
    } else if (emi.opcode.type == TwoByteOpcode) {
      switch (emi.legacy.decodeVal) {
      case 0x0:
        implemented = VEX_TWO_BYTE_OP_IMPL.count(emi.opcode.op);
        break;
      case 0x1:
        implemented = VEX_TWO_BYTE_OP_0x66_IMPL.count(emi.opcode.op);
        break;
      case 0x4:
        implemented = VEX_TWO_BYTE_OP_0xF3_IMPL.count(emi.opcode.op);
        break;
      }
    }
    if (!implemented) {
      panic("Unimplemented vex inst %#x:\n%s.\n", origPC, emi);
    }
  }
}

} // namespace X86ISA
