categories = [
    "vaddss",
    "vaddsd",
    "vaddps",
    "vaddpd",
    "vandps",
    "vcmpps",
    "vcmpss",
    "vfmadd132ps",
    "vfmadd231ps",
    "vfmadd213ss",
    "vsubss",
    "vsubsd",
    "vsubps",
    "vsubpd",
    "vminsd",
    "vmulps",
    "vmulpd",
    "vmulss",
    "vmulsd",
    "vdivss",
    "vdivsd",
    "vdivps",
    "vdivpd",
    "vxorps",
    "vxorpd",
    "vpxor",
]

microcode = '''
# AVX512 instructions
'''
for category in categories:
    exec("from . import {s} as cat".format(s=category))
    microcode += cat.microcode
