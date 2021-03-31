categories = [
    "vpaddd",
    "vpaddq",
    "vpandd",
    "vpcmpeqd",
    "vpsubq",
    "vpminsd",
    "vpminsq",
    "vpmuludq",
    "vpmulld",
    "vpslld",
    "vpsrld",
    "vpsrlq",
    "vsqrtsd",
]

microcode = '''
# AVX512 instructions
'''
for category in categories:
    exec("from . import {s} as cat".format(s=category))
    microcode += cat.microcode
