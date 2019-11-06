
from m5.objects import *

def initializeMinorCPU(options, minor_cpu):
    base_issue_width = 2
    scale = options.llvm_issue_width / base_issue_width

    minor_cpu.fetch1FetchLimit = scale * 1

    minor_cpu.fetch2InputBufferSize = scale * 2

    minor_cpu.decodeInputBufferSize = scale * 3
    minor_cpu.decodeInputWidth = options.llvm_issue_width

    minor_cpu.executeInputWidth = options.llvm_issue_width
    minor_cpu.executeIssueLimit = options.llvm_issue_width
    minor_cpu.executeMemoryIssueLimit = scale * 1
    minor_cpu.executeCommitLimit = options.llvm_issue_width
    minor_cpu.executeMemoryCommitLimit = scale * 1
    minor_cpu.executeInputBufferSize = scale * 7
    minor_cpu.executeMaxAccessesInMemory = scale * 2
    minor_cpu.executeLSQRequestsQueueSize = scale * 1
    minor_cpu.executeLSQTransfersQueueSize = scale * 2
    minor_cpu.executeLSQStoreBufferSize = scale * 5

    if options.llvm_issue_width == 2:
        minor_cpu.executeFuncUnits = MinorGemForgeI2FUPool()
    elif options.llvm_issue_width == 4:
        minor_cpu.executeFuncUnits = MinorGemForgeI4FUPool()
    else:
        print('Invalid issue width')
        assert(False)

    if options.branch_predictor == '2bit':
        minor_cpu.branchPred = LocalBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'tournament':
        minor_cpu.branchPred = TournamentBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'bimode':
        minor_cpu.branchPred = BiModeBP(
            numThreads=options.gem_forge_hardware_contexts_per_core)
    elif options.branch_predictor == 'ltage':
        minor_cpu.branchPred = LTAGE(
            numThreads=options.gem_forge_hardware_contexts_per_core)
