from m5.params import *
from m5.proxy import *

from m5.objects.BaseCPU import BaseCPU
from m5.objects.FuncUnit import *
from m5.objects.FuncUnitConfig import *
from m5.objects.FUPool import FUPool
from m5.objects.BranchPredictor import *
from m5.objects.Process import EmulatedDriver


class LLVMAccel(FUDesc):
    opList = [OpDesc(opClass='Accelerator', opLat=1, pipelined=False)]
    count = 2


class LLVMIntMultDiv(FUDesc):
    opList = [OpDesc(opClass='IntMult', opLat=4),
              OpDesc(opClass='IntDiv', opLat=12, pipelined=False)]
    count = 2

class GemForgeFPMultDiv(FUDesc):
    opList = [ OpDesc(opClass='FloatMult', opLat=4),
               OpDesc(opClass='FloatMultAcc', opLat=5),
               OpDesc(opClass='FloatMisc', opLat=3),
               OpDesc(opClass='FloatDiv', opLat=12, pipelined=False),
               OpDesc(opClass='FloatSqrt', opLat=24, pipelined=False) ]
    count = 2


class DefaultFUPool(FUPool):
    FUList = [IntALU(), LLVMIntMultDiv(), FP_ALU(),
              GemForgeFPMultDiv(), ReadPort(),
              SIMD_Unit(), WritePort(), RdWrPort(), IprPort(), LLVMAccel()]


class LLVMTraceCPU(BaseCPU):
    type = 'LLVMTraceCPU'
    cxx_header = 'cpu/gem_forge/llvm_trace_cpu.hh'

    traceFile = Param.String('', 'The input llvm trace file.')

    # Hack information of total active cpus that executes a trace.
    totalActiveCPUs = Param.Unsigned(1, 'Total number of active CPUs.')

    installMemorySnapshot = Param.Bool(True, 'Should we install memory snapshot.')
    warmCache = Param.Bool(True, 'Should we warm up the cache.')

    driver = Param.LLVMTraceCPUDriver('The driver to control this cpu.')
    maxFetchQueueSize = Param.UInt64(64, 'Maximum size of the fetch queue.')
    maxReorderBufferSize = Param.UInt64(192, 'Maximum size of the rob.')
    maxInstructionQueueSize = Param.UInt64(
        64, 'Maximum size of the instruction queue.')
    loadStoreQueueSize = Param.UInt64(
        8, 'Maximum number of load store queue.')

    fetchToDecodeDelay = Param.Cycles(1, "Fetch to decode delay")
    fetchWidth = Param.Unsigned(8, "Fetch width")
    decodeToRenameDelay = Param.Cycles(1, "Decode to rename delay")
    decodeWidth = Param.Unsigned(8, "Decode width")
    decodeQueueSize = Param.Unsigned(32, "Decode queue size")
    renameToIEWDelay = Param.Cycles(
        2, "Rename to Issue/Execute/Writeback delay")
    renameWidth = Param.Unsigned(8, "Rename width")
    renameBufferSize = Param.Unsigned(32, "Rename buffer size")
    robSize = Param.Unsigned(192, "ROB size")
    iewToCommitDelay = Param.Cycles(
        1, "Issue/Execute/Writeback to commit delay")
    dispatchWidth = Param.Unsigned(8, "dispatch width")
    issueWidth = Param.Unsigned(8, "Issue width")
    writeBackWidth = Param.Unsigned(8, "Write back width")
    instQueueSize = Param.Unsigned(32, "Inst queue size")
    loadQueueSize = Param.Unsigned(32, "Load queue size")
    storeQueueSize = Param.Unsigned(32, "Store queue size")
    commitWidth = Param.Unsigned(8, "Commit width")
    commitQueueSize = Param.Unsigned(32, "Commit queue size")

    fuPool = Param.FUPool(DefaultFUPool(), "Functional Unit pool")

    # This controls how many store instructions can be written back in one cycle.
    cacheStorePorts = Param.Unsigned(8, "Cache Store Ports. "
                                     "Constrains stores only. Loads are constrained by load FUs.")
    # This controls how many load instructions can be issued in one cycle.
    cacheLoadPorts = Param.Unsigned(4, "Cache Load Ports.")
    # Number of hardware contexts.
    hardwareContexts = Param.Unsigned(1, "Number of hardware contexts.")

    branchPred = Param.BranchPredictor(TournamentBP(numThreads=Parent.hardwareContexts),
                                       "Branch Predictor")
    useGem5BranchPredictor = Param.Bool(True, "Whether to use branch predictor from gem5.")

    # Parameters for ADFA.
    adfaEnable = Param.Bool(False, "Whether the adfa is enabled.")

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
    cxx_header = 'cpu/gem_forge/llvm_trace_cpu_driver.hh'
