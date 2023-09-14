import argparse
import os

from m5.util import addToPath, fatal

addToPath('../../')

from ruby import Ruby

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.FileSystemConfig import config_filesystem
from common.Caches import *

import GemForgeCPUConfig
import GemForgeLLVMTraceCPUConfig
import GemForgeSystem
import GemForgePrefetchConfig

parser = argparse.ArgumentParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

if '--ruby' in sys.argv:
    Ruby.define_options(parser)

def parse_tdg_files(value):
    vs = value.split(',')
    return vs

def parse_int_list(value):
    vs = [int(v) for v in value.split('x')]
    return vs

parser.add_argument("--gem-forge-work-mark-history", action="store", type=str,
                  help="""work mark history""")
parser.add_argument("--gem-forge-work-mark-switch-cpu", action="store", type=int, default=-1,
                  help="""switch cpu at this work mark (overrides m5_switch_cpu)""")
parser.add_argument("--gem-forge-work-mark-end", action="store", type=int, default=-1,
                  help="""stop at this work mark (overrides work_item_end)""")
parser.add_argument("--gem-forge-num-active-cpus", action="store", type=int,
                  help="""number of active cpus.""", default="1")
parser.add_argument("--gem-forge-enable-func-acc-tick", action="store_true",
                  help="""enable func accumulate ticks.""", default=False)
parser.add_argument("--gem-forge-enable-func-trace-at-tick", action="store", type=int,
                  help="""enable func trace at this tick.""", default=-1)
parser.add_argument("--gem-forge-cpu-deadlock-interval", action="store", type=str, default="10000ns",
                  help="""Raise deadlock in CPU after this amount of time without progress.""")
parser.add_argument("--gem-forge-empty-mem", action="store_true",
                  help="""start simulation without installing the memory snapshot.""",
                  default=False)
parser.add_argument("--gem-forge-ideal-ruby", action="store_true",
                  help="""simulate with ideal ruby cache (see Sequencer).""",
                  default=False)
parser.add_argument("--gem-forge-ruby-max-infly-data-req", action="store", type=int,
                  help="""Max inflight data reqs (see Sequencer).""",
                  default=16)
parser.add_argument("--gem-forge-ruby-max-infly-inst-req", action="store", type=int,
                  help="""Max inflight inst reqs (see Sequencer).""",
                  default=16)
parser.add_argument("--gem-forge-cold-cache", action="store_true",
                  help="""start simulation without warming up the cache.""", default=False)
parser.add_argument("--llvm-standalone", action="store_true",
                  help="""replay in stand alone mode""", default=False)
parser.add_argument("--llvm-prefetch", action="store", type=int,
                  help="""whether to use a prefetcher""", default="0")
parser.add_argument("--gem-forge-ideal-prefetcher-distance", action="store",
                  type=int, help="""whether to use an ideal prefetcher""", default=400)
parser.add_argument("--gem-forge-prefetcher", type=str, default="none",
                  choices=['none', 'stride', 'imp', 'isb', 'bingo'],
                  help="Type of L1 prefetcher we are using.")
parser.add_argument("--gem-forge-prefetch-dist", action="store", type=int,
                  help="L1 prefetcher distance", default="8")
parser.add_argument("--gem-forge-prefetch-cross-page", action="store", type=int,
                  help="Prefetcher can cross pages", default="1")
parser.add_argument("--gem-forge-prefetch-on-hit", action="store", type=int,
                  help="Prefetcher observe hits", default="0")
parser.add_argument("--gem-forge-prefetch-inst", action="store", type=int,
                  help="Prefetcher works for inst cache", default="1")
parser.add_argument("--gem-forge-prefetch-filter-dup", action="store", type=int,
                  help="Prefetcher filters out duplicate stream", default="0")
parser.add_argument("--gem-forge-l2-prefetcher", type=str, default="none",
                  choices=['none', 'stride'],
                  help="Type of L2 prefetcher we are using.")
parser.add_argument("--gem-forge-l2-prefetch-dist", action="store", type=int,
                  help="L2 prefetcher distance", default="8")
parser.add_argument("--gem-forge-l2-prefetch-cross-page", action="store", type=int,
                  help="L2 prefetcher can cross pages", default="1")
parser.add_argument("--gem-forge-l2-prefetch-on-hit", action="store", type=int,
                  help="L2 prefetcher observe hits", default="0")
parser.add_argument("--gem-forge-l2-bulk-prefetch-size", action="store", type=int,
                  help="Bulk prefetch size at L2.", default=1)
parser.add_argument("--gem-forge-prefetch-on-access", action="store_true",
                  help="""whether to prefetch on every access""", default=False)
parser.add_argument("--llvm-trace-file", type=parse_tdg_files,
                  help="""llvm trace file input LLVMTraceCPU""", default=[])
parser.add_argument("--gem-forge-core-pipeline", type=str,
                  choices=['none', 'sapphire-rapids'],
                  help="""core uarch details""", default="none")
parser.add_argument("--llvm-issue-width", action="store", type=int,
                  help="""llvm issue width""", default="8")
parser.add_argument("--llvm-store-queue-size", action="store",
                  type=int, help="""store queue size""", default="32")
parser.add_argument("--llvm-load-queue-size", action="store",
                  type=int, help="""load queue size""", default="32")
parser.add_argument("--gem-forge-cache-load-ports", action="store", type=int,
                  help="""How many loads can be issued in one cycle""", default="4")
parser.add_argument("--gem-forge-cache-store-ports", action="store", type=int,
                  help="""How many stores can be written-back in one cycle""", default="4")
parser.add_argument("--gem-forge-hardware-contexts-per-core", action="store", type=int,
                  help="""How many thread context""", default="1")
parser.add_argument("--branch-predictor", type=str, default="ltage",
                  choices=['2bit', 'tournament', 'bimode', 'ltage'],
                  help = "type of branch predictor to use")
parser.add_argument("--gem-forge-no-gem5-branch-predictor", action="store_true",
                  help="""Disable gem5 branch predictor and use our simple one""", default=False)

parser.add_argument("--llvm-mcpat", action="store", type=int,
                  help="""whether to use mcpat to estimate power""", default="0")

parser.add_argument("--gem-forge-ideal-prefetcher", action="store_true",
                  help="""whether to use an ideal prefetcher""", default=False)

parser.add_argument("--gem-forge-stream-engine-enable", action="store_true", default=False,
                  help="""Enable stream engine.""")
parser.add_argument("--gem-forge-stream-engine-default-run-ahead-length", action="store", type=int,
                  help="""How many elements can a stream run ahead""", default="4")
parser.add_argument("--gem-forge-stream-engine-total-run-ahead-length",
                  action="store", type=int,
                  help="""How many elements can the stream engine run ahead""", default="128")
parser.add_argument("--gem-forge-stream-engine-total-run-ahead-bytes",
                  action="store", type=int,
                  help="""How many bytes can the stream engine run ahead""", default="512")
parser.add_argument("--gem-forge-stream-engine-max-num-elements-prefetch-for-atomic", 
                  action="store", type=int,
                  help="""How many elements to prefech to AtomicStream""", default="1024")
parser.add_argument("--gem-forge-stream-engine-is-oracle", action="store", type=int,
                  help="""whether make the stream engine oracle""", default="0")
parser.add_argument("--gem-forge-stream-engine-throttling", action="store", type=str,
                  help="""Throttling tenchique used by stream engine.""", default="static")
parser.add_argument("--gem-forge-stream-engine-enable-lsq", action="store_true",
                  help="""Enable stream lsq in the stream engine.""", default=False)
parser.add_argument("--gem-forge-stream-engine-enable-o3-elim-stream-end", action="store_true",
                  help="""Enable out-of-order StreamEnd for eliminated stream.""",
                  default=False)
parser.add_argument("--gem-forge-stream-engine-force-no-flush-peb", action="store_true",
                  help="""Disable flush PEB in the stream engine.""", default=False)
parser.add_argument("--gem-forge-stream-engine-enable-coalesce", action="store_true",
                  help="""Enable stream coalesce in the stream engine.""", default=False)
parser.add_argument("--gem-forge-stream-engine-enable-merge", action="store_true",
                  help="""Enable stream merge in the stream engine.""", default=False)
parser.add_argument("--gem-forge-stream-engine-placement",
                  type=str, default="original")
parser.add_argument("--gem-forge-stream-engine-elim-nest-stream-instances", action="store",
                  default="8", type=int, help="""number of elim nested stream instances""")
parser.add_argument("--gem-forge-stream-engine-elim-nest-outer-stream-elems", action="store",
                  default="16", type=int, help="""number of elim nested outer stream elems""")
parser.add_argument("--gem-forge-stream-engine-yield-core-when-blocked", action="store", type=int,
                  default="0", help="""yield the core when blocked by stream engine""")

# Stream Float options.
parser.add_argument("--gem-forge-stream-engine-enable-float", action="store_true", default=False,
                  help="Enable stream float in LLC.")
parser.add_argument("--gem-forge-stream-engine-float-policy", type=str, default="static",
                  choices=['static', 'manual', 'smart', 'smart-reuse', 'smart-computation'],
                  help="Policy to choose floating stream in LLC.")
parser.add_argument("--gem-forge-stream-engine-enable-float-history", action="store", type=int,
                  default=0, help="Enable stream float history when making float decision.")
parser.add_argument("--gem-forge-stream-engine-enable-remote-elim-nest-stream-config",
                  action="store_true", default=False,
                  help="Enable eliminated nest stream directly be configured at remote bank.")
parser.add_argument("--gem-forge-stream-engine-enable-float-indirect", action="store_true",
                  default=False,
                  help="Enable indirect stream float in LLC.")
parser.add_argument("--gem-forge-stream-engine-enable-float-pseudo", action="store_true",
                  default=False,
                  help="Enable stream pseudo float in LLC.")
parser.add_argument("--gem-forge-stream-engine-enable-float-cancel", action="store_true",
                  default=False,
                  help="Enable stream float cancel if core stream engine.")
parser.add_argument("--gem-forge-stream-engine-enable-float-subline", action="store_true",
                  default=False,
                  help="Enable subline transimission in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-ack", action="store",
                  default=0, type=int,
                  help="Enable idea (instant, no NoC) StreamAck in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-ack-for-pred-off-elem",
                    action="store", default=1, type=int,
                    help="Enable idea (instant, no NoC) StreamAck for PredicateOff Elem.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-fwd", action="store",
                  default=0, type=int,
                  help="Enable idea (instant, no NoC) StreamForward in near-stream computing.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-end", action="store",
                  default=0, type=int,
                  help="Enable idea (instant, no NoC) StreamEnd in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-partial-config", action="store",
                  default=0, type=int,
                  help="Enable partial StreamConfig (only dynamic params) in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-flow", action="store",
                  default=0, type=int,
                  help="Enable idea (instant, no NoC) StreamFlow in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-ind-req", action="store",
                  default=0, type=int,
                  help="Enable idea (instant, no NoC) IndStreamReq in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-mlc-pop-check", action="store_true",
                  default=True,
                  help="When MLCStream pops, check LLCStream progress ideally.")
parser.add_argument("--gem-forge-stream-engine-enable-float-idea-store", action="store_true",
                  default=False,
                  help="Enable idea (instant, no NoC) StreamStore in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-compact-store", action="store_true",
                  default=False,
                  help="Enable compact (same cache line) StreamStore in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-advance-migrate", action="store_true",
                  default=False,
                  help="Enable advance migrate in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-multicast", action="store_true",
                  default=False,
                  help="Enable multicast transimission in stream float.")
parser.add_argument("--gem-forge-stream-engine-enable-float-multicast-forward", action="store_true",
                  default=False,
                  help="Enable multicast forwarding in stream float.")
parser.add_argument("--gem-forge-stream-engine-llc-multicast-group-size", action="store",
                  type=int, default="0",
                  help="Stream MulticastGroupSize for LLCStreamEngine.")
parser.add_argument("--gem-forge-stream-engine-llc-multicast-issue-policy", type=str,
                  default='first', choices=['any', 'first_allocated', 'first'],
                  help="Stream Multicast issue policy, first means most conservative.")
parser.add_argument("--gem-forge-stream-engine-mlc-stream-buffer-init-num-entries", action="store",
                  type=int, default="32",
                  help="# MLC slices allocated per stream.")
parser.add_argument("--gem-forge-stream-engine-mlc-ind-stream-buffer-init-num-entries", action="store",
                  type=int, default="8",
                  help="# MLC slices allocated for streams with indirect stream.")
parser.add_argument("--gem-forge-stream-engine-mlc-stream-runahead-slice-inverse-ratio",
                  action="store", type=int, default="1",
                  help="Ratio of MLC runahead slices per stream (slices / ratio).")
parser.add_argument("--gem-forge-stream-engine-mlc-stream-buffer-to-segment-ratio", action="store",
                  type=int, default="4",
                  help="Ratio between MLC stream buffer size and sync segment.")
parser.add_argument("--gem-forge-stream-engine-llc-stream-engine-issue-width", action="store",
                  type=int, default="1",
                  help="LLCStreamEngine issue width.")
parser.add_argument("--gem-forge-stream-engine-llc-stream-engine-migrate-width", action="store",
                  type=int, default="1",
                  help="LLCStreamEngine migrate width.")
parser.add_argument("--gem-forge-stream-engine-llc-stream-max-infly-request", action="store",
                  type=int, default="8",
                  help="LLCStream max infly request per stream.")
parser.add_argument("--gem-forge-stream-engine-enable-midway-float", action="store_true",
                  default=False,
                  help="Enable midway stream float.")
parser.add_argument("--gem-forge-stream-engine-midway-float-element-idx", action="store",
                  type=int, default="-1",
                  help="Force midway stream float from this element.")
parser.add_argument("--gem-forge-stream-engine-llc-max-ind-req-inqueue-per-stream", action="store",
                  type=int, default="4",
                  help="Max indirect stream request inqueue per stream.")
parser.add_argument("--gem-forge-stream-engine-llc-multicast-max-ind-req-per-message", action="store",
                  type=int, default="0",
                  help="Max indirect stream request per multicast message, 0 to disable.")
parser.add_argument("--gem-forge-stream-engine-llc-multicast-ind-req-bank-group-size", action="store",
                  type=int, default="0",
                  help="Indirect stream request multicast bank group size.")

# Stream Computing options.
parser.add_argument("--gem-forge-estimate-pure-data-traffic", action="store_true",
                  default="False",
                  help="Enable idea traffic and estimate pure data traffic.")
parser.add_argument("--gem-forge-stream-engine-compute-width", action="store",
                  type=int, default="1",
                  help="Core/LLC StreamEngine compute width.")
parser.add_argument("--gem-forge-stream-engine-llc-max-infly-computation", action="store",
                  type=int, default="32",
                  help="Max num of infly computation in LLC StreamEngine.")
parser.add_argument("--gem-forge-stream-engine-llc-access-core-simd-delay", action="store",
                  type=int, default="0",
                  help="Delay for LLC StreamEngine to access core SIMD unit.")
parser.add_argument("--gem-forge-stream-engine-has-scalar-alu", action="store",
                  type=int, default="1",
                  help="Allow SE to handle scalar computation without accessing the core.")
parser.add_argument("--gem-forge-stream-engine-mlc-generate-direct-range", action="store",
                  type=int, default="1",
                  help="MLC SE will generate DirectRange so no need for Remote SE to send.")
parser.add_argument("--gem-forge-enable-stream-zero-compute-latency", action="store_true",
                  default="False",
                  help="Core/LLC StreamEngine compute done in 0 cycle latency.")
parser.add_argument("--gem-forge-enable-llc-stream-engine-trace", action="store", type=int,
                  default="0", help="Trace LLC StreamEngine.")
parser.add_argument("--gem-forge-enable-stream-range-sync", action="store_true",
                  default="False",
                  help="Range-based synchronization between Core/LLC StreamEngine.")
parser.add_argument("--gem-forge-stream-atomic-lock-type", type=str, default="multi-reader",
                  choices=['single', 'multi-reader'],
                  help="Set StreamAtomicLockType in LLC StreamEngine.")
parser.add_argument("--gem-forge-enable-stream-float-indirect-reduction", action="store_true",
                  default="False",
                  help="Enable floating indirect reduction stream.")
parser.add_argument("--gem-forge-enable-stream-float-distributed-indirect-reduction", action="store_true",
                  default="False",
                  help="Enable performing indirect reduction stream distributively.")
parser.add_argument("--gem-forge-enable-stream-float-multi-level-indirect-store-compute",
                  action="store_true", default="False",
                  help="Enable floating multi-level indirect store compute stream.")
parser.add_argument("--gem-forge-stream-engine-llc-neighbor-stream-threshold", action="store",
                  type=int, default="0",
                  help="# of streams threshold to delay migration to neighbor LLC SE. 0 to disable.")
parser.add_argument("--gem-forge-stream-engine-llc-neighbor-migration-delay", action="store",
                  type=int, default="100",
                  help="Delay to migrate to neighbor LLC SE if reached the threshold.")
parser.add_argument("--gem-forge-stream-engine-llc-neighbor-migration-valve-type", type=str,
                  choices=['none', 'all', 'hard'], default='none',
                  help="Apply valve to all streams.")
parser.add_argument("--gem-forge-stream-engine-enable-fine-grained-near-data-computing",
                  action="store_true", default="False",
                  help="Enable per element computation offloading.")
parser.add_argument("--gem-forge-enable-mlc-prefetch-stream", type=int,
                  action="store", default="0",
                  help="Enable mlc prefetch stream.")
parser.add_argument("--gem-forge-enable-stream-nuca", type=int,
                  action="store", default="0",
                  help="Enable stream nuca.")
parser.add_argument("--gem-forge-stream-nuca-force-distribute-array", type=int,
                  action="store", default="0",
                  help="Force NUCA distribute the array.")
parser.add_argument("--gem-forge-enable-stream-strand", type=int,
                  action="store", default="0",
                  help="Enable stream strand auto parallelization.")
parser.add_argument("--gem-forge-enable-stream-strand-elem-split", type=int,
                  action="store", default="0",
                  help="Enable stream strand split by elem.")
parser.add_argument("--gem-forge-enable-stream-strand-broadcast", type=int,
                  action="store", default="0",
                  help="Enable stream strand auto broadcast.")
parser.add_argument("--gem-forge-enable-stream-vectorize", type=int,
                  action="store", default="0",
                  help="Enable stream auto vectorization.")
parser.add_argument("--gem-forge-stream-nuca-direct-region-fit-policy", type=str,
                  choices=['crop', 'drop'], default='crop',
                  help="What to do when direct regions overflow LLC.")
parser.add_argument("--gem-forge-stream-nuca-ind-remap-box-bytes", type=int,
                  action="store", default="0",
                  help="Indirect remap box size (0 to disable).")
parser.add_argument("--gem-forge-stream-nuca-ind-rebalance-threshold", type=float,
                  action="store", default="0.0",
                  help="Indirect rebalance threshold (0 to disable).")
parser.add_argument("--gem-forge-stream-nuca-enable-csr-reorder", type=int,
                  action="store", default="0",
                  help="Reorder CSR edges to reduce migration (0 to disable).")

# Stream PUM Options.
parser.add_argument("--gem-forge-stream-pum-mode", type=int,
                  action="store", default="0",
                  help="0: Disable; 1: Enable stream PUM; 2: Enable stream PUM mapping only.")
parser.add_argument("--gem-forge-stream-pum-enable-tiling", type=int,
                  action="store", default="1",
                  help="Enable PUM tiling.")
parser.add_argument("--gem-forge-stream-pum-force-tiling-dim", type=str,
                  choices=['none', 'inner', 'outer'], default='none',
                  help="Force PUM tiling on certain dimension.")
parser.add_argument("--gem-forge-stream-pum-force-tiling-size",
                  default=[], type=parse_int_list,
                  help="Force PUM tiling size on inner dimension.")
parser.add_argument("--gem-forge-stream-pum-num-bitlines", type=int,
                  action="store", default="256",
                  help="Number of bitlines per SRAM array.")
parser.add_argument("--gem-forge-stream-pum-num-wordlines", type=int,
                  action="store", default="256",
                  help="Number of wordlines per SRAM array.")
parser.add_argument("--gem-forge-stream-pum-force-data-type", type=str,
                  choices=['none', 'int', 'fp'], default='none',
                  help="Whether force PUM compute to have certain data type.")
parser.add_argument("--gem-forge-stream-pum-enable-parallel-intra-array-shift", type=int,
                  action="store", default="0",
                  help="Whether intra-array shift can happen in parallel.")
parser.add_argument("--gem-forge-stream-pum-enable-parallel-inter-array-shift", type=int,
                  action="store", default="1",
                  help="Whether inter-array shift can happen in parallel.")
parser.add_argument("--gem-forge-stream-pum-enable-parallel-way-read", type=int,
                  action="store", default="1",
                  help="Whether parallelize way read.")
parser.add_argument("--gem-forge-stream-pum-optimize-dfg", type=int,
                  action="store", default="1",
                  help="Enable tDFG optimization (e.g., merging, scheduling).")
parser.add_argument("--gem-forge-stream-pum-optimize-dfg-expand-tensor", type=int,
                  action="store", default="0",
                  help="Enable tDFG optimization tensor expansion.")
parser.add_argument("--gem-forge-stream-pum-schedule-type", type=str,
                  choices=['linear', 'bfs', 'unison'], default='bfs',
                  help="Which scheduler to use for tDFG.")
parser.add_argument("--gem-forge-stream-pum-compile-lat-per-cmd", type=int,
                  action="store", default="10",
                  help="Compiling latency per cmd.")
parser.add_argument("--gem-forge-stream-pum-prefetch-level", type=str, default="llc",
                  choices=['llc', 'mem', 'none'], help="Where to execute the PUMPrefetch stream.")
parser.add_argument("--gem-forge-stream-pum-optimized-directory",
                  action="store", type=str, default="",
                  help="Directory containing optimized tdfgs")
parser.add_argument("--gem-forge-stream-pum-fix-stream-at-req-bank",
                  action="store", type=int, default="0",
                  help=("Fix all streams at req. bank. "
                        "Used to approximate in-mem computing only (no near-stream computing)"))

# Stream in Mem Options.
parser.add_argument("--gem-forge-stream-engine-enable-float-mem", action="store_true", default=False,
                  help="Enable stream float in Mem Ctrl.")
parser.add_argument("--gem-forge-stream-engine-float-level-policy", type=str, default="manual",
                  choices=['static', 'manual', 'manual2', 'smart'],
                  help="Policy to choose floating level for streams.")
parser.add_argument("--gem-forge-stream-engine-mc-stream-max-infly-request", action="store",
                  type=int, default="16",
                  help="LLCStream max infly request per stream.")
parser.add_argument("--gem-forge-stream-engine-mc-neighbor-stream-threshold", action="store",
                  type=int, default="0",
                  help="# of streams threshold to delay migration to neighbor MCC SE. 0 to disable.")
parser.add_argument("--gem-forge-stream-engine-mc-neighbor-migration-delay", action="store",
                  type=int, default="100",
                  help="Delay to migrate to neighbor MC SE if reached the threshold.")
parser.add_argument("--gem-forge-stream-engine-mc-neighbor-migration-valve-type", type=str,
                  choices=['none', 'all', 'hard'], default='none',
                  help="Apply valve to all streams.")
parser.add_argument("--gem-forge-stream-engine-mc-reuse-buffer-lines-per-core", action="store",
                  type=int, default="0",
                  help="Number of cache lines buffered in MC SE to extract more possible reuse.")
parser.add_argument("--gem-forge-stream-reuse-tile-elems", action="store",
                  type=int, default="0",
                  help="Number of elems in reused tile.")
parser.add_argument("--gem-forge-stream-engine-mc-issue-width", action="store",
                  type=int, default="1",
                  help="Mem StreamEngine issue width.")

parser.add_argument("--gem-forge-adfa-enable",
                  action="store_true", default=False)
parser.add_argument("--gem-forge-adfa-core-issue-width", action="store", type=int, default="16")
parser.add_argument("--gem-forge-adfa-enable-speculation",
                  action="store", type=int, default="0")
parser.add_argument("--gem-forge-adfa-break-iv-dep",
                  action="store", type=int, default="0")
parser.add_argument("--gem-forge-adfa-break-rv-dep",
                  action="store", type=int, default="0")
parser.add_argument("--gem-forge-adfa-break-unrollable-ctr-dep",
                  action="store", type=int, default="0")
parser.add_argument("--gem-forge-adfa-num-banks",
                  action="store", type=int, default="1")
parser.add_argument("--gem-forge-adfa-num-ports-per-bank",
                  action="store", type=int, default="1")
parser.add_argument("--gem-forge-adfa-num-cores",
                  action="store", type=int, default="1")
parser.add_argument("--gem-forge-adfa-enable-tls",
                  action="store", type=int, default="0")
parser.add_argument("--gem-forge-adfa-ideal-mem", action="store", type=int, default="0")

parser.add_argument("--gem-forge-idea-inorder-cpu", action="store_true",
                  default=False,
                  help="Enable idea inorder cpu.")

args = parser.parse_args()

if args.cpu_type == "LLVMTraceCPU":
    fatal("The host CPU should be a normal CPU other than LLVMTraceCPU\n")

# Create the cpus.
(initial_cpus, future_cpus, test_mem_mode) = \
     GemForgeCPUConfig.initializeCPUs(args)

system = System(cpu=initial_cpus,
                mem_mode=test_mem_mode,
                mem_ranges=[AddrRange(args.mem_size)],
                cache_line_size=args.cacheline_size)
# Add future_cpus to system so that they can be instantiated.
if future_cpus:
    system.future_cpus = future_cpus

system.workload = SEWorkload.init_compatible(
    system.cpu[0].workload[0].executable
)

# Set the work count options.
Simulation.setWorkCountOptions(system, args)

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)

# Create a source clock for the system. This is used as the clock period for
# xbar and memory
system.clk_domain = SrcClockDomain(clock=args.sys_clock,
                                   voltage_domain=system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a separate clock domain for the CPUs. In case of Trace CPUs this clock
# is actually used only by the caches connected to the CPU.
system.cpu_clk_domain = SrcClockDomain(clock=args.cpu_clock,
                                       voltage_domain=system.cpu_voltage_domain)

# All cpus belong to a common cpu_clk_domain, therefore running at a common
# frequency.
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain
for cpu in future_cpus:
    cpu.clk_domain = system.cpu_clk_domain

# Assign input trace files to the Trace CPU
# system.cpu.traceFile = args.llvm_trace_file

# Configure the classic memory system options
if args.ruby:
    Ruby.create_system(args, False, system)
    assert(args.num_cpus == len(system.ruby._cpu_ports))

    system.ruby.clk_domain = \
        SrcClockDomain(clock=args.ruby_clock,
                       voltage_domain=system.voltage_domain)
    for i in range(len(system.cpu)):
        ruby_port = system.ruby._cpu_ports[i]

        # Create the interrupt controller and connect its ports to Ruby
        # Note that the interrupt controller is always present but only
        # in x86 does it have message ports that need to be connected
        system.cpu[i].createInterruptController()

        # Connect the cpu's cache ports to Ruby
        ruby_port.connectCpuPorts(system.cpu[i])
else:
    MemClass = Simulation.setMemClass(args)
    system.membus = SystemXBar()
    system.system_port = system.membus.slave
    CacheConfig.config_cache(args, system)
    MemConfig.config_mem(args, system)
    config_filesystem(system, args)

if args.llvm_mcpat == 1:
    print('McPATManager is not working anymore.')
    system.mcpat_manager = McPATManager()

# Disable snoop filter
if not args.ruby and args.l2cache:
    system.tol2bus.snoop_filter = NULL

GemForgeLLVMTraceCPUConfig.initializeStreamPolicy(args, system)
GemForgePrefetchConfig.initializePrefetch(args, system)

root = Root(full_system=False, system=system)
GemForgeSystem.run(args, root, system, future_cpus)
