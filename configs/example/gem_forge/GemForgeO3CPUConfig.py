
from m5.objects import *

def initializeO3CPU(options, o3cpu):
    o3cpu.fetchWidth = options.llvm_issue_width
    o3cpu.decodeWidth = options.llvm_issue_width
    o3cpu.renameWidth = options.llvm_issue_width
    o3cpu.dispatchWidth = options.llvm_issue_width
    o3cpu.issueWidth = options.llvm_issue_width
    o3cpu.wbWidth = options.llvm_issue_width
    o3cpu.commitWidth = options.llvm_issue_width
    if options.branch_predictor == '2bit':
        o3cpu.branchPred = LocalBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'tournament':
        o3cpu.branchPred = TournamentBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'bimode':
        o3cpu.branchPred = BiModeBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'ltage':
        o3cpu.branchPred = LTAGE(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    # Use our FU pool.
    o3cpu.fuPool = GemForgeO4FUPool()

    o3cpu.SQEntries = options.llvm_store_queue_size
    o3cpu.LQEntries = options.llvm_load_queue_size
    if options.llvm_issue_width == 2:
        o3cpu.numROBEntries = 64
        o3cpu.numIQEntries = 16
        o3cpu.LQEntries = 16
        o3cpu.SQEntries = 20
    elif options.llvm_issue_width == 4:
        o3cpu.numROBEntries = 96
        o3cpu.numIQEntries = 24
        o3cpu.LQEntries = 24
        o3cpu.SQEntries = 30
    elif options.llvm_issue_width == 6:
        o3cpu.numROBEntries = 192
        o3cpu.numIQEntries = 28
        o3cpu.LQEntries = 42
        o3cpu.SQEntries = 36