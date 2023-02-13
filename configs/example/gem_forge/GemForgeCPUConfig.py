from m5.objects import *

from m5.util import addToPath, fatal
addToPath('../../')
from common import Simulation

import GemForgeLLVMTraceCPUConfig
import GemForgeO3CPUConfig
import GemForgeMinorCPUConfig
import GemForgeAccConfig

import os

def get_processes(args):
    """Interprets provided options and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = args.cmd.split(';')
    if args.input != "":
        inputs = args.input.split(';')
    if args.output != "":
        outputs = args.output.split(';')
    if args.errout != "":
        errouts = args.errout.split(';')
    if args.options != "":
        pargs = args.options.split(';')

    idx = 0
    for wrkld in workloads:
        process = Process()
        process.executable = wrkld
        process.cwd = os.getcwd()
        process.exitGroup = True
        process.lazyAllocation = False
        # Yield wakeup every 10us.
        process.yieldWakeup = '2us'

        if len(pargs) > idx:
            process.cmd = [wrkld] + pargs[idx].split()
        else:
            process.cmd = [wrkld]

        if len(inputs) > idx:
            process.input = inputs[idx]
        if len(outputs) > idx:
            process.output = outputs[idx]
        if len(errouts) > idx:
            process.errout = errouts[idx]

        if args.gem_forge_work_mark_history:
            history = list()
            with open(args.gem_forge_work_mark_history) as f:
                for line in f:
                    if line.startswith('#'):
                        continue
                    history.append(int(line))
            process.markHistory = history
        process.markSwitchcpu = args.gem_forge_work_mark_switch_cpu
        process.markEnd = args.gem_forge_work_mark_end
        process.enableMemStream = \
            args.gem_forge_stream_engine_enable_float_mem
        process.enableStreamNUCA = \
            args.gem_forge_enable_stream_nuca
        process.streamNUCADirectRegionFitPolicy = \
            args.gem_forge_stream_nuca_direct_region_fit_policy
        process.streamNUCAIndRemapBoxBytes = \
            args.gem_forge_stream_nuca_ind_remap_box_bytes
        process.streamNUCAIndRebalanceThreshold = \
            args.gem_forge_stream_nuca_ind_rebalance_threshold
        process.streamNUCAEnableCSRReorder = \
            args.gem_forge_stream_nuca_enable_csr_reorder
        process.enableStreamPUMMapping = \
            args.gem_forge_stream_pum_mode != 0
        process.enableStreamPUMTiling = \
            args.gem_forge_stream_pum_enable_tiling
        process.forceStreamPUMTilingDim = \
            args.gem_forge_stream_pum_force_tiling_dim
        process.forceStreamPUMTilingSize = \
            args.gem_forge_stream_pum_force_tiling_size

        multiprocesses.append(process)
        idx += 1

    if args.smt:
        assert(args.cpu_type == "detailed" or args.cpu_type == "inorder")
        return multiprocesses, idx
    else:
        return multiprocesses, 1


def createCPUNonStandalone(args, CPUClass, multiprocesses, numThreads):
    if CPUClass is None:
        return list()
    CPUClass.numThreads = numThreads
    # Non-standalone mode, intialize the driver and normal cpu.

    # In this case FutureClass will be None as there is not fast forwarding or
    # switching
    cpus = [CPUClass(cpu_id=i) for i in range(args.num_cpus)]

    # Set the workload for normal CPUs.
    for i in range(args.num_cpus):
        cpu = cpus[i]
        if args.smt:
            cpu.workload = multiprocesses
        elif len(multiprocesses) == 1:
            cpu.workload = multiprocesses[0]
        else:
            cpu.workload = multiprocesses[i]
        cpu.function_acc_tick = args.gem_forge_enable_func_acc_tick
        cpu.function_trace = args.gem_forge_enable_func_trace
        cpu.createThreads()
        # Also set the common parameters for the normal CPUs.
        if isinstance(cpu, BaseO3CPU):
            GemForgeO3CPUConfig.initializeO3CPU(args, cpu)
        elif isinstance(cpu, BaseMinorCPU):
            GemForgeMinorCPUConfig.initializeMinorCPU(args, cpu)
            pass
        elif isinstance(cpu, BaseTimingSimpleCPU):
            pass
        elif isinstance(cpu, BaseAtomicSimpleCPU):
            pass
        else:
            raise ValueError("Unsupported cpu class.")

    # Add the emulated LLVM tracer driver for the process
    if args.llvm_trace_file:
        # Make sure that we have one trace file per processes
        assert(len(args.llvm_trace_file) == len(multiprocesses))
        for i in range(len(args.llvm_trace_file)):
            process = multiprocesses[i]
            tdg_fn = args.llvm_trace_file[i]

            driver = LLVMTraceCPUDriver()
            driver.filename = 'llvm_trace_cpu'
            process.drivers = [driver]

            # For each process, add a LLVMTraceCPU for simulation.
            llvm_trace_cpu = \
                GemForgeLLVMTraceCPUConfig.initializeLLVMTraceCPU(
                    options, len(cpus))

            llvm_trace_cpu.cpu_id = len(cpus)
            llvm_trace_cpu.traceFile = tdg_fn
            llvm_trace_cpu.driver = driver
            llvm_trace_cpu.totalActiveCPUs = len(multiprocesses)

            cpus.append(llvm_trace_cpu)
    args.num_cpus = len(cpus)
    return cpus

def createCPUStandalone(args):
    # Standalone mode, just the LLVMTraceCPU.
    # There should be a trace file for replay.
    assert(args.llvm_trace_file != '')
    cpus = list()
    """
    If num_cpus equals 1, we create as many cpus as traces specified.
    If there is only one trace, we create as many cpu as #num_cpus, but only assign
    #gem_forge_num_active_cpus to the same trace.
    Otherwise, panic.
    """
    assert(args.gem_forge_num_active_cpus <= args.num_cpus)
    if len(args.llvm_trace_file) == 1:
        # Duplicate the traces to num_cpus.
        args.llvm_trace_file = args.llvm_trace_file * args.num_cpus
        # Clear extra traces more than gem_forge_num_active_cpus.
        for i in range(args.gem_forge_num_active_cpus, args.num_cpus):
            args.llvm_trace_file[i] = ''
    elif args.num_cpus == 1:
        args.num_cpus = len(args.llvm_trace_file)
        args.gem_forge_num_active_cpus = args.num_cpus
    else:
        assert(args.num_cpus == len(args.llvm_trace_file))
        args.gem_forge_num_active_cpus = args.num_cpus

    for i in range(len(args.llvm_trace_file)):
        tdg_fn = args.llvm_trace_file[i]

        # For each process, add a LLVMTraceCPU for simulation.
        llvm_trace_cpu = \
            GemForgeLLVMTraceCPUConfig.initializeLLVMTraceCPU(
                options, len(cpus))

        # A dummy null driver to make the python script happy.
        llvm_trace_cpu.cpu_id = len(cpus)
        llvm_trace_cpu.createThreads()
        llvm_trace_cpu.traceFile = tdg_fn
        llvm_trace_cpu.driver = NULL
        llvm_trace_cpu.totalActiveCPUs = args.gem_forge_num_active_cpus

        # if tdg_fn != '':
        #     process = multiprocesses[i]
        #     llvm_trace_cpu.workload = process

        cpus.append(llvm_trace_cpu)
    assert(args.num_cpus == len(cpus))
    return cpus

"""
Initialize the cpus.
Notice that it supported fast forward.
"""
def initializeCPUs(args):
    (InitialCPUClass, test_mem_mode, FutureCPUClass) = \
        Simulation.setCPUClass(args)
    if args.llvm_standalone:
        assert(FutureCPUClass is None)
        initial_cpus = createCPUStandalone(args)
        future_cpus = list()
    else:
        multiprocesses, numThreads = get_processes(args)
        assert(numThreads == 1)
        initial_cpus = createCPUNonStandalone(
            args, InitialCPUClass, multiprocesses, numThreads)
        future_cpus = createCPUNonStandalone(
            args, FutureCPUClass, multiprocesses, numThreads)

    # Set up TLB options.
    for cpu in future_cpus if future_cpus else initial_cpus:
        dtb = cpu.mmu.dtb
        if isinstance(dtb, X86TLB):
            dtb.size = args.l1tlb_size
            dtb.assoc = args.l1tlb_assoc
            dtb.l2size = args.l2tlb_size
            dtb.l2assoc = args.l2tlb_assoc
            dtb.l2_lat = args.l2tlb_hit_lat
            dtb.walker_se_lat = args.walker_se_lat
            dtb.walker_se_port = args.walker_se_port
            dtb.timing_se = args.tlb_timing_se

    # We initialize GemForge for initial_cpus.
    for cpu in initial_cpus:
        cpu.accelManager = \
            GemForgeAccConfig.initializeGemForgeAcceleratorManager(args)
        cpu.yield_latency = args.cpu_yield_latency
        if args.prog_interval:
            cpu.progress_interval = '100' # Hz
        cpu.deadlock_interval = args.gem_forge_cpu_deadlock_interval
    # We initialize empty GemForge for future_cpus.
    for cpu in future_cpus:
        cpu.accelManager = \
            GemForgeAccConfig.initializeEmptyGemForgeAcceleratorManager(args)
        cpu.yield_latency = args.cpu_yield_latency
        if args.prog_interval:
            cpu.progress_interval = args.prog_interval
        cpu.deadlock_interval = args.gem_forge_cpu_deadlock_interval
        # Estimate pure data traffic for future cpu.
        cpu.enableIdeaCache = args.gem_forge_estimate_pure_data_traffic

    for cpu in future_cpus if future_cpus else initial_cpus:
        if args.gem_forge_idea_inorder_cpu:
            cpu.enableIdeaInorderCPU = True
    for cpu in future_cpus:
        cpu.switched_out = True
    # Update the progress count.
    # if args.prog_interval:
    #     for cpu in initial_cpus:
    #         cpu.progress_interval = args.prog_interval
    #     if future_cpus:
    #         for cpu in future_cpus:
    #             cpu.progress_interval = args.prog_interval


    return (initial_cpus, future_cpus, test_mem_mode)


