categories = [
    "vpaddd",
    "vpaddq",
    "vpsubq",
    "vpminsd",
    "vpminsq",
]

microcode = '''
# AVX512 instructions
'''
for category in categories:
    exec "import %s as cat" % category
    microcode += cat.microcode
