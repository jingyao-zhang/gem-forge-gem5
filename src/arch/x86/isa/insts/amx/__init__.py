categories = [
    "data_transfer",
    "compute",
]

microcode = """
# AMX instructions
"""
for category in categories:
    exec("from . import {s} as cat".format(s=category))
    microcode += cat.microcode
