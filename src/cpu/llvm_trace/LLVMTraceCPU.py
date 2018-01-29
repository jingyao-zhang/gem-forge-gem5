from m5.params import *

from BaseCPU import BaseCPU
from FUPool import DefaultFUPool
from Process import EmulatedDriver

class LLVMTraceCPU(BaseCPU):
    type = 'LLVMTraceCPU'
    cxx_header = 'cpu/llvm_trace/llvm_trace_cpu.hh'

    traceFile = Param.String('', 'The input llvm trace file.')
    driver = Param.LLVMTraceCPUDriver('The driver to control this cpu.')
    maxFetchQueueSize = Param.UInt64(8, 'Maximum size of the fetch queue.')
    maxReorderBufferSize = Param.UInt64(8, 'Maximum size of the rob.')

    fuPool = Param.FUPool(DefaultFUPool(), "Functional Unit pool")

    @classmethod
    def memory_mode(cls):
        return 'timing'

    @classmethod
    def require_caches(cls):
        return False

    @classmethod
    def support_take_over(cls):
        return False

class LLVMTraceCPUDriver(EmulatedDriver):
    type = 'LLVMTraceCPUDriver'
    cxx_header = 'cpu/llvm_trace/llvm_trace_cpu_driver.hh'

