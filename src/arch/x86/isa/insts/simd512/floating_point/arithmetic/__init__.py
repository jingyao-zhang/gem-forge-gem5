categories = [
    "vaddss",
    "vaddsd",
    "vaddps",
    "vsubss",
    "vsubsd",
    "vsubps",
    "vmulps",
    "vmulss",
    "vmulsd",
    "vdivss",
    "vdivsd",
    "vdivps",
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
