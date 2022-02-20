# Copyright (c) 2017,2019 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2009 Advanced Micro Devices, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.params import *
from m5.proxy import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.Sequencer import RubySequencer

class RubyController(ClockedObject):
    type = 'RubyController'
    cxx_class = 'AbstractController'
    cxx_header = "mem/ruby/slicc_interface/AbstractController.hh"
    abstract = True
    version = Param.Int("")
    router_id = Param.Int(-1, "RounterId of this controller. -1 means Invalid.")
    numa_banks = VectorParam.Int([], "Banks that handled by this NUMA node.")
    addr_ranges = VectorParam.AddrRange([AllMemory], "Address range this "
                                        "controller responds to")
    cluster_id = Param.UInt32(0, "Id of this controller's cluster")

    transitions_per_cycle = \
        Param.Int(32, "no. of  SLICC state machine transitions per cycle")
    buffer_size = Param.UInt32(0, "max buffer size 0 means infinite")

    recycle_latency = Param.Cycles(10, "")
    number_of_TBEs = Param.Int(256, "")
    ruby_system = Param.RubySystem("")

    # This is typically a proxy to the icache/dcache hit latency.
    # If the latency depends on the request type or protocol-specific states,
    # the protocol may ignore this parameter by overriding the
    # mandatoryQueueLatency function
    mandatory_queue_latency = \
        Param.Cycles(1, "Default latency for requests added to the " \
                        "mandatory queue on top-level controllers")

    memory = MasterPort("Port for attaching a memory controller")
    system = Param.System(Parent.any, "system object parameter")
    sequencer = Param.RubySequencer(NULL,"CPU sequencer attached to cache")

# ! Sean: StreamAwareCache
class RubyStreamAwareController(RubyController):
    type = 'RubyStreamAwareController'
    cxx_class = 'AbstractStreamAwareController'
    cxx_header = 'mem/ruby/slicc_interface/AbstractStreamAwareController.hh'
    abstract = True

    # ! Sean: StreamAwareCache
    # ! I don't think the addr_ranges work in MESI_Three_Level.
    # ! I will hack here by adding a llc_select_low_bit and llc_select_num_bits.
    # ! This only works for a S-NUCA.
    llc_select_low_bit = Param.UInt32(0, "Low bit used to select LLC bank")
    llc_select_num_bits = Param.UInt32(0, "Num of bits used to select LLC bank")
    # So far we only support Mesh topology
    num_cores_per_row = Param.UInt32(0, "Num of cores per row for Mesh Topology")
    enable_stream_float = Param.Bool(False, "Whether to enable stream float.")
    enable_stream_subline = Param.Bool(False, "Whether to enable stream float subline transmission.")
    enable_stream_partial_config = Param.Bool(False, "Whether to enable partial StreamConfig.")
    enable_stream_idea_ack = Param.Bool(False, "Whether to enable immediate StreamAck.")
    enable_stream_idea_end = Param.Bool(False, "Whether to enable immediate StreamEnd.")
    enable_stream_idea_flow = Param.Bool(False, "Whether to enable immediate stream flow control.")
    enable_stream_idea_store = Param.Bool(False, "Whether to enable immediate stream store.")
    enable_stream_compact_store = Param.Bool(False, "Whether to enable compact stream store.")
    enable_stream_advance_migrate = Param.Bool(False, "Whether to enable advance stream migrate.")
    enable_stream_multicast = Param.Bool(False, "Whether to enable multicast stream.")
    stream_multicast_group_size = Param.UInt32(0, "MulticastGroup is Size x Size, 0 means all.")
    stream_multicast_issue_policy = \
        Param.String("any", "Multicast issue policy, default is the relaxed.")
    ind_stream_req_max_per_multicast_msg = \
        Param.UInt32(0, "Max ind req per multicast msg, >= 2 to enable.")
    ind_stream_req_multicast_group_size = \
        Param.UInt32(0, "Ind req multicast group is Size x Size, 0 means all.")
    mlc_stream_buffer_init_num_entries = \
        Param.UInt32(16, "Initial number of entries of MLC stream buffer.")
    mlc_stream_buffer_to_segment_ratio = \
        Param.UInt32(4, "Ratio between MLC buffer and segment.")
    enable_mlc_stream_idea_pop_check_llc_progress = \
        Param.Bool(True, "When MLCStream pop, ideally check LLCStream progress.")
    llc_stream_engine_issue_width = \
        Param.UInt32(1, "Issue width of LLCStreamEngine.")
    llc_stream_engine_migrate_width = \
        Param.UInt32(1, "Issue width of LLCStreamEngine.")
    llc_stream_max_infly_request = \
        Param.UInt32(8, "Max infly requests per LLC stream.")
    enable_stream_llc_issue_clear = Param.Bool(True, "Whether to enable llc stream issue clear.")
    llc_stream_engine_compute_width = \
        Param.UInt32(1, "Compute width of LLCStreamEngine.")
    llc_stream_engine_max_infly_computation = \
        Param.UInt32(32, "Max num of infly computation in LLCStreamEngine.")
    enable_llc_stream_zero_compute_latency = Param.Bool(False, "Whether to enable zero compute latency.")
    enable_stream_range_sync = Param.Bool(False, "Whether to enable stream range synchronization.")
    stream_atomic_lock_type = Param.String("none", "StreamAtomicLockType of none, single, multi-reader.")
    llc_access_core_simd_delay = Param.UInt32(4, "Latency to access core simd unit.")
    has_scalar_alu = Param.Bool(True, "Whether SE has scalar ALU to avoid going to the core.")
    mlc_generate_direct_range = Param.Bool(True,
        "Whether MLC SE generates DirectRanges so that Remote SE can issue Ranges ideally.")

    neighbor_stream_threshold = \
        Param.UInt32(0, "Number of streams to trigger migration control. 0 to disable.")
    neighbor_migration_delay = \
        Param.UInt32(100, "Number of cycles delay per migrating streams.")
    neighbor_migration_valve_type = \
        Param.String("none", "Migration valve type.")

    enable_stream_float_mem = Param.Bool(False, "Whether to enable stream float to mem ctrl.")
    reuse_buffer_lines_per_core = Param.UInt32(0, "Number of cache lines per core in the reuse buffer.")

    enable_stream_strand = Param.Bool(False, "Whether to enable stream strand auto parallelization.")
