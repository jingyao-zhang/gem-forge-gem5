import optparse
import os

from m5.util import addToPath, fatal

addToPath('../../')

from ruby import Ruby

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.Caches import *

import GemForgeCPUConfig
import GemForgeLLVMTraceCPUConfig
import GemForgeSystem
import GemForgePrefetchConfig

parser = optparse.OptionParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

if '--ruby' in sys.argv:
    Ruby.define_options(parser)

def parse_tdg_files(option, opt, value, parser):
    vs = value.split(',')
    setattr(parser.values, option.dest, vs)

parser.add_option("--gem-forge-work-mark-history", action="store", type="string",
                  help="""work mark history""")
parser.add_option("--gem-forge-work-mark-switch-cpu", action="store", type="int", default=-1,
                  help="""switch cpu at this work mark (overrides m5_switch_cpu)""")
parser.add_option("--gem-forge-work-mark-end", action="store", type="int", default=-1,
                  help="""stop at this work mark (overrides work_item_end)""")
parser.add_option("--gem-forge-num-active-cpus", action="store", type="int",
                  help="""number of active cpus.""", default="1")
parser.add_option("--gem-forge-enable-func-acc-tick", action="store_true",
                  help="""enable func accumulate ticks.""", default=False)
parser.add_option("--gem-forge-cpu-deadlock-interval", action="store", type="string", default="200000ns",
                  help="""Raise deadlock in CPU 0 after this amount of time without progress.""")
parser.add_option("--gem-forge-empty-mem", action="store_true",
                  help="""start simulation without installing the memory snapshot.""",
                  default=False)
parser.add_option("--gem-forge-ideal-ruby", action="store_true",
                  help="""simulate with ideal ruby cache (see Sequencer).""",
                  default=False)
parser.add_option("--gem-forge-cold-cache", action="store_true",
                  help="""start simulation without warming up the cache.""", default=False)
parser.add_option("--llvm-standalone", action="store_true",
                  help="""replay in stand alone mode""", default=False)
parser.add_option("--llvm-prefetch", action="store", type="int",
                  help="""whether to use a prefetcher""", default="0")
parser.add_option("--gem-forge-ideal-prefetcher-distance", action="store",
                  type="int", help="""whether to use an ideal prefetcher""", default=400)
parser.add_option("--gem-forge-prefetcher", type="choice", default="none",
                  choices=['none', 'stride', 'imp', 'isb', 'bingo'],
                  help="Type of L1 prefetcher we are using.")
parser.add_option("--gem-forge-prefetch-dist", action="store", type="int",
                  help="L1 prefetcher distance", default="8")
parser.add_option("--gem-forge-l2-prefetcher", type="choice", default="none",
                  choices=['none', 'stride'],
                  help="Type of L2 prefetcher we are using.")
parser.add_option("--gem-forge-l2-prefetch-dist", action="store", type="int",
                  help="L2 prefetcher distance", default="8")
parser.add_option("--gem-forge-l2-bulk-prefetch-size", action="store", type="int",
                  help="Bulk prefetch size at L2.", default=1)
parser.add_option("--gem-forge-prefetch-on-access", action="store_true",
                  help="""whether to prefetch on every access""", default=False)
parser.add_option("--llvm-trace-file", action="callback", type="string",
                  help="""llvm trace file input LLVMTraceCPU""", default="",
                  callback=parse_tdg_files)
parser.add_option("--llvm-issue-width", action="store", type="int",
                  help="""llvm issue width""", default="8")
parser.add_option("--llvm-store-queue-size", action="store",
                  type="int", help="""store queue size""", default="32")
parser.add_option("--llvm-load-queue-size", action="store",
                  type="int", help="""load queue size""", default="32")
parser.add_option("--gem-forge-cache-load-ports", action="store", type="int",
                  help="""How many loads can be issued in one cycle""", default="4")
parser.add_option("--gem-forge-cache-store-ports", action="store", type="int",
                  help="""How many stores can be written-back in one cycle""", default="4")
parser.add_option("--gem-forge-hardware-contexts-per-core", action="store", type="int",
                  help="""How many thread context""", default="1")
parser.add_option("--branch-predictor", type="choice", default="ltage",
                  choices=['2bit', 'tournament', 'bimode', 'ltage'],
                  help = "type of branch predictor to use")
parser.add_option("--gem-forge-no-gem5-branch-predictor", action="store_true",
                  help="""Disable gem5 branch predictor and use our simple one""", default=False)

parser.add_option("--llvm-mcpat", action="store", type="int",
                  help="""whether to use mcpat to estimate power""", default="0")

parser.add_option("--gem-forge-ideal-prefetcher", action="store_true",
                  help="""whether to use an ideal prefetcher""", default=False)

parser.add_option("--gem-forge-stream-engine-enable", action="store_true", default=False,
                  help="""Enable stream engine.""")
parser.add_option("--gem-forge-stream-engine-default-run-ahead-length", action="store", type="int",
                  help="""How many elements can a stream run ahead""", default="4")
parser.add_option("--gem-forge-stream-engine-total-run-ahead-length",
                  action="store", type="int",
                  help="""How many elements can the stream engine run ahead""", default="128")
parser.add_option("--gem-forge-stream-engine-total-run-ahead-bytes",
                  action="store", type="int",
                  help="""How many bytes can the stream engine run ahead""", default="512")
parser.add_option("--gem-forge-stream-engine-max-num-elements-prefetch-for-atomic", 
                  action="store", type="int",
                  help="""How many elements to prefech to AtomicStream""", default="1024")
parser.add_option("--gem-forge-stream-engine-is-oracle", action="store", type="int",
                  help="""whether make the stream engine oracle""", default="0")
parser.add_option("--gem-forge-stream-engine-throttling", action="store", type="string",
                  help="""Throttling tenchique used by stream engine.""", default="static")
parser.add_option("--gem-forge-stream-engine-enable-lsq", action="store_true",
                  help="""Enable stream lsq in the stream engine.""", default=False)
parser.add_option("--gem-forge-stream-engine-force-no-flush-peb", action="store_true",
                  help="""Disable flush PEB in the stream engine.""", default=False)
parser.add_option("--gem-forge-stream-engine-enable-coalesce", action="store_true",
                  help="""Enable stream coalesce in the stream engine.""", default=False)
parser.add_option("--gem-forge-stream-engine-enable-merge", action="store_true",
                  help="""Enable stream merge in the stream engine.""", default=False)
parser.add_option("--gem-forge-stream-engine-placement",
                  type="string", default="original")

# Stream Float options.
parser.add_option("--gem-forge-stream-engine-enable-float", action="store_true", default=False,
                  help="Enable stream float in LLC.")
parser.add_option("--gem-forge-stream-engine-float-policy", type="choice", default="static",
                  choices=['static', 'manual', 'smart', 'smart-computation'],
                  help="Policy to choose floating stream in LLC.")
parser.add_option("--gem-forge-stream-engine-enable-float-indirect", action="store_true",
                  default=False,
                  help="Enable indirect stream float in LLC.")
parser.add_option("--gem-forge-stream-engine-enable-float-pseudo", action="store_true",
                  default=False,
                  help="Enable stream pseudo float in LLC.")
parser.add_option("--gem-forge-stream-engine-enable-float-cancel", action="store_true",
                  default=False,
                  help="Enable stream float cancel if core stream engine.")
parser.add_option("--gem-forge-stream-engine-enable-float-subline", action="store_true",
                  default=False,
                  help="Enable subline transimission in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-idea-ack", action="store_true",
                  default=False,
                  help="Enable idea (instant, no NoC) StreamAck in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-idea-sync", action="store_true",
                  default=False,
                  help="Enable idea (instant, no NoC) sync in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-idea-flow", action="store_true",
                  default=False,
                  help="Enable idea (instant, no NoC) StreamFlow in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-idea-store", action="store_true",
                  default=False,
                  help="Enable idea (instant, no NoC) StreamStore in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-compact-store", action="store_true",
                  default=False,
                  help="Enable compact (same cache line) StreamStore in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-advance-migrate", action="store_true",
                  default=False,
                  help="Enable advance migrate in stream float.")
parser.add_option("--gem-forge-stream-engine-enable-float-multicast", action="store_true",
                  default=False,
                  help="Enable multicast transimission in stream float.")
parser.add_option("--gem-forge-stream-engine-llc-multicast-group-size", action="store",
                  type="int", default="0",
                  help="Stream MulticastGroupSize for LLCStreamEngine.")
parser.add_option("--gem-forge-stream-engine-llc-multicast-issue-policy", type="choice",
                  default='first', choices=['any', 'first_allocated', 'first'],
                  help="Stream Multicast issue policy, first means most conservative.")
parser.add_option("--gem-forge-stream-engine-mlc-stream-buffer-init-num-entries", action="store",
                  type="int", default="32",
                  help="Initial number of entries of MLC stream buffer per stream.")
parser.add_option("--gem-forge-stream-engine-mlc-stream-buffer-to-segment-ratio", action="store",
                  type="int", default="4",
                  help="Ratio between MLC stream buffer size and sync segment.")
parser.add_option("--gem-forge-stream-engine-llc-stream-engine-issue-width", action="store",
                  type="int", default="1",
                  help="LLCStreamEngine issue width.")
parser.add_option("--gem-forge-stream-engine-llc-stream-engine-migrate-width", action="store",
                  type="int", default="1",
                  help="LLCStreamEngine migrate width.")
parser.add_option("--gem-forge-stream-engine-llc-stream-max-infly-request", action="store",
                  type="int", default="8",
                  help="LLCStream max infly request per stream.")

# Stream Computing options.
parser.add_option("--gem-forge-estimate-pure-data-traffic", action="store_true",
                  default="False",
                  help="Enable idea traffic and estimate pure data traffic.")
parser.add_option("--gem-forge-stream-engine-compute-width", action="store",
                  type="int", default="1",
                  help="Core/LLC StreamEngine compute width.")
parser.add_option("--gem-forge-stream-engine-llc-max-infly-computation", action="store",
                  type="int", default="32",
                  help="Max num of infly computation in LLC StreamEngine.")
parser.add_option("--gem-forge-stream-engine-llc-access-core-simd-delay", action="store",
                  type="int", default="0",
                  help="Delay for LLC StreamEngine to access core SIMD unit.")
parser.add_option("--gem-forge-enable-stream-zero-compute-latency", action="store_true",
                  default="False",
                  help="Core/LLC StreamEngine compute done in 0 cycle latency.")
parser.add_option("--gem-forge-enable-stream-range-sync", action="store_true",
                  default="False",
                  help="Range-based synchronization between Core/LLC StreamEngine.")
parser.add_option("--gem-forge-stream-atomic-lock-type", type="choice", default="none",
                  choices=['none', 'single', 'multi-reader'],
                  help="Set StreamAtomicLockType in LLC StreamEngine.")
parser.add_option("--gem-forge-enable-stream-float-indirect-reduction", action="store_true",
                  default="False",
                  help="Enable floating indirect reduction stream.")
parser.add_option("--gem-forge-enable-stream-float-two-level-indirect-store-compute",
                  action="store_true", default="False",
                  help="Enable floating two-level indirect store compute stream.")
parser.add_option("--gem-forge-stream-engine-llc-neighbor-stream-threshold", action="store",
                  type="int", default="0",
                  help="# of streams threshold to delay migration to neighbor LLC SE. 0 to disable.")
parser.add_option("--gem-forge-stream-engine-llc-neighbor-migration-delay", action="store",
                  type="int", default="100",
                  help="Delay to migrate to neighbor LLC SE.")
parser.add_option("--gem-forge-stream-engine-enable-fine-grained-near-data-computing",
                  action="store_true", default="False",
                  help="Enable per element computation offloading.")

parser.add_option("--gem-forge-adfa-enable",
                  action="store_true", default=False)
parser.add_option("--gem-forge-adfa-core-issue-width", action="store", type="int", default="16")
parser.add_option("--gem-forge-adfa-enable-speculation",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-break-iv-dep",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-break-rv-dep",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-break-unrollable-ctr-dep",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-num-banks",
                  action="store", type="int", default="1")
parser.add_option("--gem-forge-adfa-num-ports-per-bank",
                  action="store", type="int", default="1")
parser.add_option("--gem-forge-adfa-num-cores",
                  action="store", type="int", default="1")
parser.add_option("--gem-forge-adfa-enable-tls",
                  action="store", type="int", default="0")
parser.add_option("--gem-forge-adfa-ideal-mem", action="store", type="int", default="0")

parser.add_option("--gem-forge-idea-inorder-cpu", action="store_true",
                  default=False,
                  help="Enable idea inorder cpu.")

(options, args) = parser.parse_args()

if args:
    fatal("Error: script doesn't take any positional arguments")

if options.cpu_type == "LLVMTraceCPU":
    fatal("The host CPU should be a normal CPU other than LLVMTraceCPU\n")

# Create the cpus.
(initial_cpus, future_cpus, test_mem_mode) = \
     GemForgeCPUConfig.initializeCPUs(options)

system = System(cpu=initial_cpus,
                mem_mode=test_mem_mode,
                mem_ranges=[AddrRange(options.mem_size)],
                cache_line_size=options.cacheline_size)
# Add future_cpus to system so that they can be instantiated.
if future_cpus:
    system.future_cpus = future_cpus

# Set the work count options.
Simulation.setWorkCountOptions(system, options)

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage=options.sys_voltage)

# Create a source clock for the system. This is used as the clock period for
# xbar and memory
system.clk_domain = SrcClockDomain(clock=options.sys_clock,
                                   voltage_domain=system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a separate clock domain for the CPUs. In case of Trace CPUs this clock
# is actually used only by the caches connected to the CPU.
system.cpu_clk_domain = SrcClockDomain(clock=options.cpu_clock,
                                       voltage_domain=system.cpu_voltage_domain)

# All cpus belong to a common cpu_clk_domain, therefore running at a common
# frequency.
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain
for cpu in future_cpus:
    cpu.clk_domain = system.cpu_clk_domain

# Assign input trace files to the Trace CPU
# system.cpu.traceFile = options.llvm_trace_file

# Configure the classic memory system options
if options.ruby:
    Ruby.create_system(options, False, system)
    assert(options.num_cpus == len(system.ruby._cpu_ports))

    system.ruby.clk_domain = \
        SrcClockDomain(clock=options.ruby_clock,
                       voltage_domain=system.voltage_domain)
    for i in range(len(system.cpu)):
        ruby_port = system.ruby._cpu_ports[i]

        # Create the interrupt controller and connect its ports to Ruby
        # Note that the interrupt controller is always present but only
        # in x86 does it have message ports that need to be connected
        system.cpu[i].createInterruptController()

        # Connect the cpu's cache ports to Ruby
        system.cpu[i].icache_port = ruby_port.slave
        system.cpu[i].dcache_port = ruby_port.slave
        if buildEnv['TARGET_ISA'] == 'x86':
            system.cpu[i].interrupts[0].pio = ruby_port.master
            system.cpu[i].interrupts[0].int_master = ruby_port.slave
            system.cpu[i].interrupts[0].int_slave = ruby_port.master
            system.cpu[i].itb.walker.port = ruby_port.slave
            system.cpu[i].dtb.walker.port = ruby_port.slave
else:
    MemClass = Simulation.setMemClass(options)
    system.membus = SystemXBar()
    system.system_port = system.membus.slave
    CacheConfig.config_cache(options, system)
    MemConfig.config_mem(options, system)

if options.llvm_mcpat == 1:
    system.mcpat_manager = McPATManager()

# Disable snoop filter
if not options.ruby and options.l2cache:
    system.tol2bus.snoop_filter = NULL

GemForgeLLVMTraceCPUConfig.initializeStreamPolicy(options, system)
GemForgePrefetchConfig.initializePrefetch(options, system)

root = Root(full_system=False, system=system)
GemForgeSystem.run(options, root, system, future_cpus)
