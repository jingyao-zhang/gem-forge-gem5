
categories = [
    "tileloadd",
    "tilestored",
]

microcode = """
# AMX Tile move instructions
"""
for category in categories:
    exec("from . import {s} as cat".format(s=category))
    microcode += cat.microcode
