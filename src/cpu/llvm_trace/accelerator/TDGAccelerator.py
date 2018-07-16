from m5.params import *
from m5.SimObject import SimObject

class TDGAcceleratorManager(SimObject):
    type = 'TDGAcceleratorManager'
    cxx_header = 'cpu/llvm_trace/accelerator/tdg_accelerator.hh'