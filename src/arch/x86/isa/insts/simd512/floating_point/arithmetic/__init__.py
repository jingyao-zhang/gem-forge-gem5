categories = [
    "vaddss",
    "vaddps",
    "vsubss",
    "vsubps",
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
