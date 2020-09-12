
from m5.objects import *

def initializeADFA(options):
    adfa = AbstractDataFlowAccelerator()
    adfa.adfaCoreIssueWidth = options.gem_forge_adfa_core_issue_width
    adfa.adfaEnableSpeculation = options.gem_forge_adfa_enable_speculation
    adfa.adfaBreakIVDep = options.gem_forge_adfa_break_iv_dep
    adfa.adfaBreakRVDep = options.gem_forge_adfa_break_rv_dep
    adfa.adfaBreakUnrollableControlDep = \
        options.gem_forge_adfa_break_unrollable_ctr_dep
    adfa.adfaNumBanks = options.gem_forge_adfa_num_banks
    adfa.adfaNumPortsPerBank = options.gem_forge_adfa_num_ports_per_bank
    adfa.adfaNumCores = options.gem_forge_adfa_num_cores
    adfa.adfaEnableTLS = (options.gem_forge_adfa_enable_tls == 1)
    adfa.adfaIdealMem = (options.gem_forge_adfa_ideal_mem == 1)
    return adfa

def initializeIdealPrefetcher(options):
    idealPrefetcher = IdealPrefetcher()
    idealPrefetcher.enableIdealPrefetcher = options.gem_forge_ideal_prefetcher
    idealPrefetcher.idealPrefetcherDistance = options.gem_forge_ideal_prefetcher_distance
    return idealPrefetcher

def initializeStreamEngine(options):
    se = StreamEngine()
    se.streamEngineIsOracle = (
        options.gem_forge_stream_engine_is_oracle != 0)
    se.streamEngineMaxRunAHeadLength = (
        options.gem_forge_stream_engine_max_run_ahead_length
    )
    se.streamEngineMaxTotalRunAHeadLength = (
        options.gem_forge_stream_engine_max_total_run_ahead_length
    )
    se.streamEngineThrottling = options.gem_forge_stream_engine_throttling
    se.streamEngineEnableLSQ = options.gem_forge_stream_engine_enable_lsq
    se.streamEngineEnableCoalesce = options.gem_forge_stream_engine_enable_coalesce
    se.streamEngineEnableMerge = options.gem_forge_stream_engine_enable_merge

    se.streamEngineEnableFloat = options.gem_forge_stream_engine_enable_float
    se.streamEngineFloatPolicy = options.gem_forge_stream_engine_float_policy
    se.streamEngineEnableFloatIndirect = \
        options.gem_forge_stream_engine_enable_float_indirect
    se.streamEngineEnableFloatPseudo = \
        options.gem_forge_stream_engine_enable_float_pseudo
    se.streamEngineEnableFloatCancel = \
        options.gem_forge_stream_engine_enable_float_cancel
    if options.gem_forge_stream_engine_enable_float_indirect:
        assert(options.gem_forge_stream_engine_enable_float)
    if options.gem_forge_stream_engine_enable_float_pseudo:
        assert(options.gem_forge_stream_engine_enable_float_indirect)

    return se

def initializeEmptyGemForgeAcceleratorManager(options):
    has_accelerator = False
    if options.gem_forge_adfa_enable:
        has_accelerator = True
    if options.gem_forge_ideal_prefetcher:
        has_accelerator = True
    # accelerators.append(SpeculativePrecomputationManager(options))
    if options.gem_forge_stream_engine_enable:
        has_accelerator = True
    if has_accelerator:
        return GemForgeAcceleratorManager(accelerators=list())
    elif options.gem_forge_idea_inorder_cpu:
        # IdeaInorderCPU is implemented in CPU delegator, which is
        # dependent on accelManaguer.
        return GemForgeAcceleratorManager(accelerators=list())
    else:
        # Disable this in default.
        return NULL

def initializeGemForgeAcceleratorManager(options):
    accelerators = list()
    if options.gem_forge_adfa_enable:
        accelerators.append(initializeADFA(options))
    if options.gem_forge_ideal_prefetcher:
        accelerators.append(initializeIdealPrefetcher(options))
    # accelerators.append(SpeculativePrecomputationManager(options))
    if options.gem_forge_stream_engine_enable:
        accelerators.append(initializeStreamEngine(options))
    if accelerators:
        return GemForgeAcceleratorManager(accelerators=accelerators)
    elif options.gem_forge_idea_inorder_cpu:
        # IdeaInorderCPU is implemented in CPU delegator, which is
        # dependent on accelManaguer.
        return GemForgeAcceleratorManager(accelerators=accelerators)
    else:
        # Disable this in default.
        return NULL
