
from m5.objects import *

def initializeADFA(args):
    adfa = AbstractDataFlowAccelerator()
    adfa.adfaCoreIssueWidth = args.gem_forge_adfa_core_issue_width
    adfa.adfaEnableSpeculation = args.gem_forge_adfa_enable_speculation
    adfa.adfaBreakIVDep = args.gem_forge_adfa_break_iv_dep
    adfa.adfaBreakRVDep = args.gem_forge_adfa_break_rv_dep
    adfa.adfaBreakUnrollableControlDep = \
        args.gem_forge_adfa_break_unrollable_ctr_dep
    adfa.adfaNumBanks = args.gem_forge_adfa_num_banks
    adfa.adfaNumPortsPerBank = args.gem_forge_adfa_num_ports_per_bank
    adfa.adfaNumCores = args.gem_forge_adfa_num_cores
    adfa.adfaEnableTLS = (args.gem_forge_adfa_enable_tls == 1)
    adfa.adfaIdealMem = (args.gem_forge_adfa_ideal_mem == 1)
    return adfa

def initializeIdealPrefetcher(args):
    idealPrefetcher = IdealPrefetcher()
    idealPrefetcher.enableIdealPrefetcher = args.gem_forge_ideal_prefetcher
    idealPrefetcher.idealPrefetcherDistance = args.gem_forge_ideal_prefetcher_distance
    return idealPrefetcher

def initializeStreamEngine(args):
    se = StreamEngine()
    se.streamEngineIsOracle = (
        args.gem_forge_stream_engine_is_oracle != 0)
    se.defaultRunAheadLength = \
        args.gem_forge_stream_engine_default_run_ahead_length
    se.totalRunAheadLength = \
        args.gem_forge_stream_engine_total_run_ahead_length
    se.totalRunAheadBytes = \
        args.gem_forge_stream_engine_total_run_ahead_bytes
    se.maxNumElementsPrefetchForAtomic = \
        args.gem_forge_stream_engine_max_num_elements_prefetch_for_atomic
    se.throttling = args.gem_forge_stream_engine_throttling
    se.streamEngineEnableLSQ = args.gem_forge_stream_engine_enable_lsq
    se.enableO3ElimStreamEnd = \
        args.gem_forge_stream_engine_enable_o3_elim_stream_end
    se.streamEngineForceNoFlushPEB = args.gem_forge_stream_engine_force_no_flush_peb
    se.streamEngineEnableCoalesce = args.gem_forge_stream_engine_enable_coalesce
    se.streamEngineEnableMerge = args.gem_forge_stream_engine_enable_merge
    se.elimNestStreamInstances = \
        args.gem_forge_stream_engine_elim_nest_stream_instances
    se.elimNestOuterStreamElems = \
        args.gem_forge_stream_engine_elim_nest_outer_stream_elems

    se.streamEngineEnableFloat = args.gem_forge_stream_engine_enable_float
    se.streamEngineFloatPolicy = args.gem_forge_stream_engine_float_policy
    se.enableFloatHistory = args.gem_forge_stream_engine_enable_float_history
    se.streamEngineEnableFloatIndirect = \
        args.gem_forge_stream_engine_enable_float_indirect
    se.streamEngineEnableFloatPseudo = \
        args.gem_forge_stream_engine_enable_float_pseudo
    se.streamEngineEnableFloatCancel = \
        args.gem_forge_stream_engine_enable_float_cancel
    if args.gem_forge_stream_engine_enable_float_indirect:
        assert(args.gem_forge_stream_engine_enable_float)
    if args.gem_forge_stream_engine_enable_float_pseudo:
        assert(args.gem_forge_stream_engine_enable_float_indirect)
    se.mlc_stream_buffer_init_num_entries = \
        args.gem_forge_stream_engine_mlc_stream_buffer_init_num_entries

    se.streamEngineEnableMidwayFloat = \
        args.gem_forge_stream_engine_enable_midway_float
    se.streamEngineMidwayFloatElementIdx = \
        args.gem_forge_stream_engine_midway_float_element_idx

    se.computeWidth =\
        args.gem_forge_stream_engine_compute_width
    # So far we reuse the LLC SIMD delay parameter.
    simd_delay =\
        args.gem_forge_stream_engine_llc_access_core_simd_delay
    if simd_delay >= 2:
        # Half the latency for LLC SIMD Delay, as we are closer to core.
        simd_delay = simd_delay // 2
    se.computeSIMDDelay = simd_delay
    se.hasScalarALU = args.gem_forge_stream_engine_has_scalar_alu
    se.computeMaxInflyComputation =\
        args.gem_forge_stream_engine_llc_max_infly_computation
    se.enableZeroComputeLatency =\
        args.gem_forge_enable_stream_zero_compute_latency
    se.enableRangeSync =\
        args.gem_forge_enable_stream_range_sync
    se.enableFloatIndirectReduction =\
        args.gem_forge_enable_stream_float_indirect_reduction
    se.enableFloatMultiLevelIndirectStoreCompute =\
        args.gem_forge_enable_stream_float_multi_level_indirect_store_compute
    se.enableFineGrainedNearDataComputing =\
        args.gem_forge_stream_engine_enable_fine_grained_near_data_computing

    se.enableFloatMem =\
        args.gem_forge_stream_engine_enable_float_mem
    se.floatLevelPolicy = args.gem_forge_stream_engine_float_level_policy

    return se

def initializeEmptyGemForgeAcceleratorManager(args):
    has_accelerator = False
    if args.gem_forge_adfa_enable:
        has_accelerator = True
    if args.gem_forge_ideal_prefetcher:
        has_accelerator = True
    # accelerators.append(SpeculativePrecomputationManager(args))
    if args.gem_forge_stream_engine_enable:
        has_accelerator = True
    if has_accelerator:
        return GemForgeAcceleratorManager(accelerators=list())
    elif args.gem_forge_idea_inorder_cpu:
        # IdeaInorderCPU is implemented in CPU delegator, which is
        # dependent on accelManaguer.
        return GemForgeAcceleratorManager(accelerators=list())
    else:
        # Disable this in default.
        return NULL

def initializeGemForgeAcceleratorManager(args):
    accelerators = list()
    if args.gem_forge_adfa_enable:
        accelerators.append(initializeADFA(args))
    if args.gem_forge_ideal_prefetcher:
        accelerators.append(initializeIdealPrefetcher(args))
    # accelerators.append(SpeculativePrecomputationManager(args))
    if args.gem_forge_stream_engine_enable:
        accelerators.append(initializeStreamEngine(args))
    if accelerators:
        return GemForgeAcceleratorManager(accelerators=accelerators)
    elif args.gem_forge_idea_inorder_cpu:
        # IdeaInorderCPU is implemented in CPU delegator, which is
        # dependent on accelManaguer.
        return GemForgeAcceleratorManager(accelerators=accelerators)
    else:
        # Disable this in default.
        return NULL
