from m5.params import *
from m5.SimObject import SimObject

class GemForgeAccelerator(SimObject):
    type = 'GemForgeAccelerator'
    abstract = True
    cxx_header = 'cpu/gem_forge/accelerator/gem_forge_accelerator.hh'

class AbstractDataFlowAccelerator(GemForgeAccelerator):
    type = "AbstractDataFlowAccelerator"
    cxx_header = 'cpu/gem_forge/accelerator/adfa/adfa.hh'

    adfaCoreIssueWidth = Param.Unsigned(16, "Issue width for each abstract dataflow core.")
    adfaEnableSpeculation = Param.Bool(
        False, "Whether the adfa can speculate.")
    adfaBreakIVDep = Param.Bool(
        False, "Whether the adfa can break induction variable dependence.")
    adfaBreakRVDep = Param.Bool(
        False, "Whether the adfa can break reduction variable dependence.")
    adfaBreakUnrollableControlDep = Param.Bool(
        False, "Whether the adfa can break unrollable control dependence.")
    adfaNumBanks = Param.Unsigned(1, "Adfa number of banks to cache.")
    adfaNumPortsPerBank = Param.Unsigned(
        1, "Adfa number of ports per bank to cache.")
    adfaNumCores = Param.Unsigned(1, "Adfa number of cores.")
    adfaEnableTLS = Param.Bool(False, "Whether we enable TLS for adfa.")
    adfaIdealMem = Param.Bool(False, "Whether we use an ideal memory.")

class IdealPrefetcher(GemForgeAccelerator):
    type = "IdealPrefetcher"
    cxx_header = 'cpu/gem_forge/accelerator/ideal_prefetcher/ideal_prefetcher.hh'
    enableIdealPrefetcher = Param.Bool(
        False, "Whether the ideal prefetcher is enabled.")
    idealPrefetcherDistance = Param.Unsigned(
        400, "Number of instructions to prefetch.")

class SpeculativePrecomputationManager(GemForgeAccelerator):
    type = "SpeculativePrecomputationManager"
    cxx_header = \
        'cpu/gem_forge/accelerator/speculative_precomputation/speculative_precomputation_manager.hh'

class StreamEngine(GemForgeAccelerator):
    type = "StreamEngine"
    cxx_header = 'cpu/gem_forge/accelerator/stream/stream_engine.hh'
    streamEngineIsOracle = Param.Bool(
        False, "Whether the stream engine is oracle.")
    defaultRunAheadLength = Param.Unsigned(
        10, "Default (without throttling) nubmer of elements can a stream run ahead.")
    totalRunAheadLength = Param.Unsigned(
        1000, "How many total elements to run ahead.")
    totalRunAheadBytes = Param.Unsigned(
        512, "How many bytes to run ahead (default 8 cache lines).")
    throttling = Param.String(
        "Static", "Which throttling technique to use.")
    maxNumElementsPrefetchForAtomic = Param.Unsigned(
        1024, "How many elements to prefetch for atomic stream (default 1024 = no limit).")
    streamEngineEnableLSQ = Param.Bool(
        False, "Whether the stream engine model inserting into the LSQ.")
    streamEngineForceNoFlushPEB = Param.Bool(
        False, "Whether force not flush PEB, only used for debugging.")
    streamEngineEnableCoalesce = Param.Bool(
        False, "Whether the steam engine enable coalesced streams.")
    streamEngineEnableMerge = Param.Bool(
        False, "Whether the steam engine enable stream merging.")
    streamEngineEnablePlacement = Param.Bool(
        False, "Whether the stream engine enable stream placement.")
    streamEngineEnablePlacementOracle = Param.Bool(
        False, "Whether the stream engine enable stream placement oracle.")
    streamEngineEnablePlacementBus = Param.Bool(
        False, "Whether the stream engine should consider the bus when bypassing.")
    streamEngineNoBypassingStore = Param.Bool(
        False, "Whether the stream engine should bypass store.")
    streamEngineContinuousStore = Param.Bool(
        False, "Whether the stream engine should optimize the continuous store.")
    streamEnginePeriodReset = Param.Bool(
        False, "Whether the stream engine should periodly reset the placement decision.")
    streamEnginePlacementLat = Param.String(
        "", "The latency modeling of bypassing.")
    streamEnginePlacement = Param.String(
        "placement", "Which placement techinque to use.")

    # parameters for stream float.
    streamEngineEnableFloat = Param.Bool(
        False, "Whether the stream float is enabled.")
    streamEngineFloatPolicy = Param.String(
        "static", "Policy to choose floating stream.")
    streamEngineEnableFloatIndirect = Param.Bool(
        False, "Whether the stream float is enabled for indirect stream.")
    streamEngineEnableFloatPseudo = Param.Bool(
        False, "Whether the stream float is enabled for pseudo float.")
    streamEngineEnableFloatCancel = Param.Bool(
        False, "Whether the stream float can be cancelled in the middle.")

    # parameters for stream computing.
    enableZeroComputeLatency = Param.Bool(
        False, "Whether stream computation charge zero latency")
    computeWidth = Param.Unsigned(
        1, "How many computation can be started per cycle")
    computeSIMDDelay = Param.Unsigned(
        0, "How many cycles' delay to access Core SIMD unit")
    computeMaxInflyComputation = Param.Unsigned(
        32, "Maximum number of computations infly")
    enableRangeSync = Param.Bool(False,
        "Whether enable range-based synchronization between core and LLC SE.")
    enableFloatIndirectReduction = Param.Bool(False,
        "Whether indirect reduction streams can be floated.")
    enableFloatTwoLevelIndirectStoreCompute = Param.Bool(False,
        "Whether two-level indirect store compute stream can be floated.")

class GemForgeAcceleratorManager(SimObject):
    type = 'GemForgeAcceleratorManager'
    cxx_header = 'cpu/gem_forge/accelerator/gem_forge_accelerator.hh'
    accelerators = VectorParam.GemForgeAccelerator("Accelerators.")