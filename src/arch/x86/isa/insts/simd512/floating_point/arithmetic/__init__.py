categories = [
    "vaddss",
    "vaddsd",
    "vaddps",
    "vaddpd",
    "vsubss",
    "vsubsd",
    "vsubps",
    "vsubpd",
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
    exec "import %s as cat" % category
    microcode += cat.microcode
