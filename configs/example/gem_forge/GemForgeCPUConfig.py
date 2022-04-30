from m5.objects import *

from m5.util import addToPath, fatal
addToPath('../../')
from common import Simulation

import GemForgeLLVMTraceCPUConfig
import GemForgeO3CPUConfig
import GemForgeMinorCPUConfig
import GemForgeAccConfig

import os

def get_processes(options):
    """Interprets provided options and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = options.cmd.split(';')
    if options.input != "":
        inputs = options.input.split(';')
    if options.output != "":
        outputs = options.output.split(';')
    if options.errout != "":
        errouts = options.errout.split(';')
    if options.options != "":
        pargs = options.options.split(';')

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

        if options.gem_forge_work_mark_history:
            history = list()
            with open(options.gem_forge_work_mark_history) as f:
                for line in f:
                    if line.startswith('#'):
                        continue
                    history.append(int(line))
            process.markHistory = history
        process.markSwitchcpu = options.gem_forge_work_mark_switch_cpu
        process.markEnd = options.gem_forge_work_mark_end
        process.enableMemStream = \
            options.gem_forge_stream_engine_enable_float_mem
        process.enableStreamNUCA = \
            options.gem_forge_enable_stream_nuca
        process.streamNUCADirectRegionFitPolicy = \
            options.gem_forge_stream_nuca_direct_region_fit_policy
        process.streamNUCAEnableIndPageRemap = \
            options.gem_forge_stream_nuca_enable_ind_page_remap
        process.enableStreamPUMMapping = \
            options.gem_forge_stream_pum_mode != 0
        process.enableStreamPUMTiling = \
            options.gem_forge_stream_pum_enable_tiling

        multiprocesses.append(process)
        idx += 1

    if options.smt:
        assert(options.cpu_type == "detailed" or options.cpu_type == "inorder")
        return multiprocesses, idx
    else:
        return multiprocesses, 1


def createCPUNonStandalone(options, CPUClass, multiprocesses, numThreads):
    if CPUClass is None:
        return list()
    CPUClass.numThreads = numThreads
    # Non-standalone mode, intialize the driver and normal cpu.

    # In this case FutureClass will be None as there is not fast forwarding or
    # switching
    cpus = [CPUClass(cpu_id=i) for i in range(options.num_cpus)]

    # Set the workload for normal CPUs.
    for i in range(options.num_cpus):
        cpu = cpus[i]
        if options.smt:
            cpu.workload = multiprocesses
        elif len(multiprocesses) == 1:
            cpu.workload = multiprocesses[0]
        else:
            cpu.workload = multiprocesses[i]
        cpu.function_acc_tick = options.gem_forge_enable_func_acc_tick
        cpu.createThreads()
        # Also set the common parameters for the normal CPUs.
        if isinstance(cpu, DerivO3CPU):
            GemForgeO3CPUConfig.initializeO3CPU(options, cpu)
        elif isinstance(cpu, MinorCPU):
            GemForgeMinorCPUConfig.initializeMinorCPU(options, cpu)
            pass
        elif isinstance(cpu, TimingSimpleCPU):
            pass
        elif isinstance(cpu, AtomicSimpleCPU):
            pass
        else:
            raise ValueError("Unsupported cpu class.")

    # Add the emulated LLVM tracer driver for the process
    if options.llvm_trace_file:
        # Make sure that we have one trace file per processes
        assert(len(options.llvm_trace_file) == len(multiprocesses))
        for i in range(len(options.llvm_trace_file)):
            process = multiprocesses[i]
            tdg_fn = options.llvm_trace_file[i]

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
    options.num_cpus = len(cpus)
    return cpus

def createCPUStandalone(options, multiprocesses, numThreads):
    # Standalone mode, just the LLVMTraceCPU.
    # There should be a trace file for replay.
    assert(options.llvm_trace_file != '')
    cpus = list()
    """
    If num_cpus equals 1, we create as many cpus as traces specified.
    If there is only one trace, we create as many cpu as #num_cpus, but only assign
    #gem_forge_num_active_cpus to the same trace.
    Otherwise, panic.
    """
    assert(len(multiprocesses) == len(options.llvm_trace_file))
    assert(options.gem_forge_num_active_cpus <= options.num_cpus)
    if len(options.llvm_trace_file) == 1:
        # Duplicate the traces to num_cpus.
        options.llvm_trace_file = options.llvm_trace_file * options.num_cpus
        # Clear extra traces more than gem_forge_num_active_cpus.
        for i in range(options.gem_forge_num_active_cpus, options.num_cpus):
            options.llvm_trace_file[i] = ''
    elif options.num_cpus == 1:
        options.num_cpus = len(options.llvm_trace_file)
        options.gem_forge_num_active_cpus = options.num_cpus
    else:
        assert(options.num_cpus == len(options.llvm_trace_file))
        options.gem_forge_num_active_cpus = options.num_cpus

    for i in range(len(options.llvm_trace_file)):
        tdg_fn = options.llvm_trace_file[i]

        # For each process, add a LLVMTraceCPU for simulation.
        llvm_trace_cpu = \
            GemForgeLLVMTraceCPUConfig.initializeLLVMTraceCPU(
                options, len(cpus))

        # A dummy null driver to make the python script happy.
        llvm_trace_cpu.cpu_id = len(cpus)
        llvm_trace_cpu.createThreads()
        llvm_trace_cpu.traceFile = tdg_fn
        llvm_trace_cpu.driver = NULL
        llvm_trace_cpu.totalActiveCPUs = options.gem_forge_num_active_cpus

        if tdg_fn != '':
            process = multiprocesses[i]
            llvm_trace_cpu.workload = process

        cpus.append(llvm_trace_cpu)
    assert(options.num_cpus == len(cpus))
    return cpus

"""
Initialize the cpus.
Notice that it supported fast forward.
"""
def initializeCPUs(options):
    (InitialCPUClass, test_mem_mode, FutureCPUClass) = \
        Simulation.setCPUClass(options)
    multiprocesses, numThreads = get_processes(options)
    assert(numThreads == 1)
    if options.llvm_standalone:
        assert(FutureCPUClass is None)
        initial_cpus = createCPUStandalone(
            options, multiprocesses, numThreads)
        future_cpus = list()
    else:
        initial_cpus = createCPUNonStandalone(
            options, InitialCPUClass, multiprocesses, numThreads)
        future_cpus = createCPUNonStandalone(
            options, FutureCPUClass, multiprocesses, numThreads)

    # Set up TLB options.
    for cpu in future_cpus if future_cpus else initial_cpus:
        dtb = cpu.dtb
        if isinstance(dtb, X86TLB):
            dtb.size = options.l1tlb_size
            dtb.assoc = options.l1tlb_assoc
            dtb.l2size = options.l2tlb_size
            dtb.l2assoc = options.l2tlb_assoc
            dtb.l2_lat = options.l2tlb_hit_lat
            dtb.walker_se_lat = options.walker_se_lat
            dtb.walker_se_port = options.walker_se_port
            dtb.timing_se = options.tlb_timing_se

    # We initialize GemForge for initial_cpus.
    for cpu in initial_cpus:
        cpu.accelManager = \
            GemForgeAccConfig.initializeGemForgeAcceleratorManager(options)
        cpu.yield_latency = options.cpu_yield_latency
        if options.prog_interval:
            cpu.progress_interval = '100' # Hz
        cpu.deadlock_interval = options.gem_forge_cpu_deadlock_interval
    # We initialize empty GemForge for future_cpus.
    for cpu in future_cpus:
        cpu.accelManager = \
            GemForgeAccConfig.initializeEmptyGemForgeAcceleratorManager(options)
        cpu.yield_latency = options.cpu_yield_latency
        if options.prog_interval:
            cpu.progress_interval = options.prog_interval
        cpu.deadlock_interval = options.gem_forge_cpu_deadlock_interval
        # Estimate pure data traffic for future cpu.
        cpu.enableIdeaCache = options.gem_forge_estimate_pure_data_traffic

    for cpu in future_cpus if future_cpus else initial_cpus:
        if options.gem_forge_idea_inorder_cpu:
            cpu.enableIdeaInorderCPU = True
    for cpu in future_cpus:
        cpu.switched_out = True
    # Update the progress count.
    # if options.prog_interval:
    #     for cpu in initial_cpus:
    #         cpu.progress_interval = options.prog_interval
    #     if future_cpus:
    #         for cpu in future_cpus:
    #             cpu.progress_interval = options.prog_interval


    return (initial_cpus, future_cpus, test_mem_mode)


