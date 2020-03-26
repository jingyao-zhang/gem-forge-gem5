
from m5.objects import *

def initializeLLVMTraceCPU(options, cpu_id):
    llvm_trace_cpu = LLVMTraceCPU(cpu_id=cpu_id)
    llvm_trace_cpu.fetchWidth = options.llvm_issue_width
    llvm_trace_cpu.decodeWidth = options.llvm_issue_width
    llvm_trace_cpu.renameWidth = options.llvm_issue_width
    llvm_trace_cpu.dispatchWidth = options.llvm_issue_width
    llvm_trace_cpu.issueWidth = options.llvm_issue_width
    llvm_trace_cpu.writeBackWidth = options.llvm_issue_width
    llvm_trace_cpu.commitWidth = options.llvm_issue_width
    llvm_trace_cpu.hardwareContexts = options.gem_forge_hardware_contexts_per_core
    llvm_trace_cpu.useGem5BranchPredictor = not options.gem_forge_no_gem5_branch_predictor

    if options.branch_predictor == '2bit':
        llvm_trace_cpu.branchPred = LocalBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'tournament':
        llvm_trace_cpu.branchPred = TournamentBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'bimode':
        llvm_trace_cpu.branchPred = BiModeBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'ltage':
        llvm_trace_cpu.branchPred = LTAGE(
            numThreads=options.gem_forge_hardware_contexts_per_core)


    llvm_trace_cpu.installMemorySnapshot = not options.gem_forge_empty_mem
    llvm_trace_cpu.warmCache = not options.gem_forge_cold_cache

    # ADFA options.
    llvm_trace_cpu.adfaEnable = options.gem_forge_adfa_enable

    llvm_trace_cpu.storeQueueSize = options.llvm_store_queue_size
    llvm_trace_cpu.loadQueueSize = options.llvm_load_queue_size
    if options.llvm_issue_width == 2:
        llvm_trace_cpu.robSize = 64
        llvm_trace_cpu.instQueueSize = 16
        llvm_trace_cpu.loadQueueSize = 16
        llvm_trace_cpu.storeQueueSize = 20
    elif options.llvm_issue_width == 4:
        llvm_trace_cpu.robSize = 168
        llvm_trace_cpu.instQueueSize = 24
        llvm_trace_cpu.loadQueueSize = 64
        llvm_trace_cpu.storeQueueSize = 36
    elif options.llvm_issue_width == 6:
        llvm_trace_cpu.robSize = 192
        llvm_trace_cpu.instQueueSize = 28
        llvm_trace_cpu.loadQueueSize = 42
        llvm_trace_cpu.storeQueueSize = 36
    return llvm_trace_cpu


def initializeStreamPolicy(options, system):
    # ! Very likely this will not work any more.
    if options.ruby:
        return
    for cpu in system.cpu:
        if options.gem_forge_stream_engine_placement == 'aware-replace':
            # stream-aware cache must be used with stream merge.
            # assert(options.gem_forge_stream_engine_enable_merge == 1)
            cpu.dcache.stream_aware_replacement = True
        elif options.gem_forge_stream_engine_placement == 'aware-lru':
            cpu.dcache.tags = StreamLRU()
            # if options.l1_5dcache:
            #     cpuel1_5dcache.tags = StreamLRU()
        elif options.gem_forge_stream_engine_placement == 'aware-miss-spec':
            assert(options.gem_forge_stream_engine_enable_merge)
            cpu.dcache.stream_aware_miss_speculation = True
            cpu.dcache.stream_aware_replacement = True
        elif options.gem_forge_stream_engine_placement.startswith('placement'):
            # Enable the stream-aware port for all caches and l2bus.
            cpu.dcache.use_stream_aware_cpu_port = True
            if hasattr(cpu, 'l1_5dcache'):
                cpu.l1_5dcache.use_stream_aware_cpu_port = True
            system.tol2bus.use_stream_aware_cpu_port = True
            system.l2.use_stream_aware_cpu_port = True

            cpu.streamEngineEnablePlacement = True
            if '#' in options.gem_forge_stream_engine_placement:
                x, y = options.gem_forge_stream_engine_placement.split('#')
            else:
                x = options.gem_forge_stream_engine_placement
                y = ''
            cpu.streamEnginePlacement = x
            if x == 'placement-oracle':
                cpu.streamEngineEnablePlacementOracle = True
            if 'lru' in y:
                cpu.dcache.tags = StreamLRU()
            if 'sub' in y:
                cpu.streamEnginePlacementLat = 'sub'
            elif 'imm' in y:
                cpu.streamEnginePlacementLat = 'imm'
            if 'bus' in y:
                cpu.streamEngineEnablePlacementBus = True
            if 'nst' in y:
                cpu.streamEngineNoBypassingStore = True
            if 'cst' in y:
                cpu.streamEngineContinuousStore = True
            if 'rst' in y:
                cpu.streamEnginePeriodReset = True
