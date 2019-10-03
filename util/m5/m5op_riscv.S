
/**
 * Limit support for M5Op in RISCV. Use it with caution.
 * M5Op is a special instruction defined RISCV decoder.isa.
 * Func is encoded in simm12.
 * SubFunc is not supported.
 * Arguments are always 0 (see pseudo_inst.cc)
 */

#include <gem5/asm/generic/m5ops.h>

.text

.macro m5op_func name, func, subfunc
        .align 4
        .globl \name
\name:
        .4byte 0x700b | (\func << 20)
        ret
.endm

.text
#define M5OP(name, func, subfunc) m5op_func name, func, subfunc
        M5OP_FOREACH