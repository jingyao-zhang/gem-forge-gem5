categories = [
    "vaddss",
    "vaddsd",
    "vaddps",
    "vmaxps",
    "vaddpd",
    "vandps",
    "vcmpps",
    "vcmpss",
    "vfmadd132ps",
    "vfmadd132ss",
    "vfmadd231ps",
    "vfmadd213ps",
    "vfmadd213ss",
    "vfmadd231ss",
    "vfnmadd213ps",
    "vfnmadd213ss",
    "vfnmadd231ps",
    "vfnmadd231ss",
    "vsubss",
    "vsubsd",
    "vsubps",
    "vsubpd",
    "vminsd",
    "vminss",
    "vmaxss",
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
