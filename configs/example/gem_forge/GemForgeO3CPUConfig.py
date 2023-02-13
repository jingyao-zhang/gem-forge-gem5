
from m5.objects import *

def initializeO3CPU(args, o3cpu):
    o3cpu.fetchWidth = args.llvm_issue_width
    o3cpu.decodeWidth = args.llvm_issue_width
    o3cpu.renameWidth = args.llvm_issue_width
    o3cpu.dispatchWidth = args.llvm_issue_width
    o3cpu.issueWidth = args.llvm_issue_width
    o3cpu.wbWidth = args.llvm_issue_width
    o3cpu.commitWidth = args.llvm_issue_width
    if args.branch_predictor == '2bit':
        o3cpu.branchPred = LocalBP(
            numThreads=args.gem_forge_hardware_contexts_per_core)
    elif args.branch_predictor == 'tournament':
        o3cpu.branchPred = TournamentBP(
            numThreads=args.gem_forge_hardware_contexts_per_core)
    elif args.branch_predictor == 'bimode':
        o3cpu.branchPred = BiModeBP(
            numThreads=args.gem_forge_hardware_contexts_per_core)
    elif args.branch_predictor == 'ltage':
        o3cpu.branchPred = LTAGE(
            numThreads=args.gem_forge_hardware_contexts_per_core)

    o3cpu.SQEntries = args.llvm_store_queue_size
    o3cpu.LQEntries = args.llvm_load_queue_size
    # Check LSQ at 4 byte granularity.
    o3cpu.LSQDepCheckShift = 2
    if args.llvm_issue_width == 2:
        o3cpu.numROBEntries = 64
        o3cpu.numIQEntries = 16
        o3cpu.LQEntries = 16
        o3cpu.SQEntries = 20
        o3cpu.fuPool = GemForgeO4FUPool()
    elif args.llvm_issue_width == 4:
        o3cpu.numROBEntries = 96
        o3cpu.numIQEntries = 24
        o3cpu.LQEntries = 24
        o3cpu.SQEntries = 30
        o3cpu.fuPool = GemForgeO4FUPool()
    elif args.llvm_issue_width == 6:
        o3cpu.numROBEntries = 192
        o3cpu.numIQEntries = 28
        o3cpu.LQEntries = 42
        o3cpu.SQEntries = 36
        o3cpu.fuPool = GemForgeO4FUPool()
    elif args.llvm_issue_width == 8:
        o3cpu.numROBEntries = 224
        o3cpu.numIQEntries = 64
        o3cpu.LQEntries = 72
        o3cpu.SQEntries = 56
        o3cpu.fuPool = GemForgeO8FUPool()