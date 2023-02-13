
from m5.objects import *

def initializeLLVMTraceCPU(args, cpu_id):
    llvm_trace_cpu = LLVMTraceCPU(cpu_id=cpu_id)
    llvm_trace_cpu.fetchWidth = args.llvm_issue_width
    llvm_trace_cpu.decodeWidth = args.llvm_issue_width
    llvm_trace_cpu.renameWidth = args.llvm_issue_width
    llvm_trace_cpu.dispatchWidth = args.llvm_issue_width
    llvm_trace_cpu.issueWidth = args.llvm_issue_width
    llvm_trace_cpu.writeBackWidth = args.llvm_issue_width
    llvm_trace_cpu.commitWidth = args.llvm_issue_width
    llvm_trace_cpu.hardwareContexts = args.gem_forge_hardware_contexts_per_core
    llvm_trace_cpu.useGem5BranchPredictor = not args.gem_forge_no_gem5_branch_predictor

    if args.branch_predictor == '2bit':
        llvm_trace_cpu.branchPred = LocalBP(
            numThreads=args.gem_forge_hardware_contexts_per_core)
    elif args.branch_predictor == 'tournament':
        llvm_trace_cpu.branchPred = TournamentBP(
            numThreads=args.gem_forge_hardware_contexts_per_core)
    elif args.branch_predictor == 'bimode':
        llvm_trace_cpu.branchPred = BiModeBP(
            numThreads=args.gem_forge_hardware_contexts_per_core)
    elif args.branch_predictor == 'ltage':
        llvm_trace_cpu.branchPred = LTAGE(
            numThreads=args.gem_forge_hardware_contexts_per_core)


    llvm_trace_cpu.installMemorySnapshot = not args.gem_forge_empty_mem
    llvm_trace_cpu.warmCache = not args.gem_forge_cold_cache

    # ADFA options.
    llvm_trace_cpu.adfaEnable = args.gem_forge_adfa_enable

    llvm_trace_cpu.storeQueueSize = args.llvm_store_queue_size
    llvm_trace_cpu.loadQueueSize = args.llvm_load_queue_size
    if args.llvm_issue_width == 2:
        llvm_trace_cpu.robSize = 64
        llvm_trace_cpu.instQueueSize = 16
        llvm_trace_cpu.loadQueueSize = 16
        llvm_trace_cpu.storeQueueSize = 20
    elif args.llvm_issue_width == 4:
        llvm_trace_cpu.robSize = 168
        llvm_trace_cpu.instQueueSize = 24
        llvm_trace_cpu.loadQueueSize = 64
        llvm_trace_cpu.storeQueueSize = 36
    elif args.llvm_issue_width == 6:
        llvm_trace_cpu.robSize = 192
        llvm_trace_cpu.instQueueSize = 28
        llvm_trace_cpu.loadQueueSize = 42
        llvm_trace_cpu.storeQueueSize = 36
    return llvm_trace_cpu


def initializeStreamPolicy(args, system):
    # ! Very likely this will not work any more.
    if args.ruby:
        return
    for cpu in system.cpu:
        if args.gem_forge_stream_engine_placement == 'aware-replace':
            # stream-aware cache must be used with stream merge.
            # assert(args.gem_forge_stream_engine_enable_merge == 1)
            cpu.dcache.stream_aware_replacement = True
        elif args.gem_forge_stream_engine_placement == 'aware-lru':
            cpu.dcache.tags = StreamLRU()
            # if args.l1_5dcache:
            #     cpuel1_5dcache.tags = StreamLRU()
        elif args.gem_forge_stream_engine_placement == 'aware-miss-spec':
            assert(args.gem_forge_stream_engine_enable_merge)
            cpu.dcache.stream_aware_miss_speculation = True
            cpu.dcache.stream_aware_replacement = True
        elif args.gem_forge_stream_engine_placement.startswith('placement'):
            # Enable the stream-aware port for all caches and l2bus.
            cpu.dcache.use_stream_aware_cpu_port = True
            if hasattr(cpu, 'l1_5dcache'):
                cpu.l1_5dcache.use_stream_aware_cpu_port = True
            system.tol2bus.use_stream_aware_cpu_port = True
            system.l2.use_stream_aware_cpu_port = True

            cpu.streamEngineEnablePlacement = True
            if '#' in args.gem_forge_stream_engine_placement:
                x, y = args.gem_forge_stream_engine_placement.split('#')
            else:
                x = args.gem_forge_stream_engine_placement
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
