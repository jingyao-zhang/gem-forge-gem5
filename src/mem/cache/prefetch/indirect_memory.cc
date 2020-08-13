/**
 * Copyright (c) 2018 Metempsy Technology Consulting
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

 #include "mem/cache/prefetch/indirect_memory.hh"

 #include "mem/cache/base.hh"
 #include "mem/cache/prefetch/associative_set_impl.hh"
 #include "params/IndirectMemoryPrefetcher.hh"
 #include "debug/IMPrefetcher.hh"

namespace Prefetcher {

void
IndirectMemory::insertIndex(int64_t index, PrefetchTableEntry &entry)
{
    // Enabled entry, update the index
    // this->index = index;
    // if (!this->increasedIndirectCounter) {
    //     this->indirectCounter--;
    // } else {
    //     // Set this to false, to see if the new index
    //     // has any match
    //     this->increasedIndirectCounter = false;
    // }
    auto &index_queue = entry.index_queue;
    if (index_queue.size() == this->indexQueueSize) {
        const auto &retire_index = index_queue.front();
        if (!retire_index.matched) {
            // This index has no match found.
            entry.indirectCounter--;
        }
        index_queue.pop_front();
    }
    index_queue.emplace_back(index, false /* matched */);
}

void
IndirectMemory::checkAccessMatchOnIndex(Addr pc, Addr addr, PrefetchTableEntry &entry)
{
    // Addr indirect_addr = entry.baseAddr + (entry.index << entry.shift);
    // bool match = indirect_addr == addr;
    // DPRINTF(IMPrefetcher,
    //     "pc %#x pt_pc %#x %#x %s %#x(%#x + %d << %d) counter %d.\n",
    //     pc, entry.getTag(), addr, match ? "==" : "!=", indirect_addr,
    //     entry.baseAddr, entry.index, entry.shift,
    //     static_cast<int>(entry.indirectCounter));
    // if (match) {
    //     entry.indirectCounter++;
    //     entry.increasedIndirectCounter = true;
    // }
    for (auto &index_match : entry.index_queue) {
        auto index = index_match.index;
        Addr indirect_addr = entry.baseAddr + (index << entry.shift);
        bool match = indirect_addr == addr;
        DPRINTF(IMPrefetcher,
            "pc %#x pt_pc %#x %#x %s %#x(%#x + %d << %d) counter %d.\n",
            pc, entry.getTag(), addr, match ? "==" : "!=", indirect_addr,
            entry.baseAddr, index, entry.shift,
            static_cast<int>(entry.indirectCounter));
        if (match) {
            entry.indirectCounter++;
            entry.increasedIndirectCounter = true;
            index_match.matched = true;
        }
    }
}

IndirectMemory::IndirectMemory(const IndirectMemoryPrefetcherParams *p)
  : Queued(p),
    maxPrefetchDistance(p->max_prefetch_distance),
    shiftValues(p->shift_values), prefetchThreshold(p->prefetch_threshold),
    streamCounterThreshold(p->stream_counter_threshold),
    streamingDistance(p->streaming_distance),
    indexQueueSize(p->index_queue_size),
    prefetchTable(p->pt_table_assoc, p->pt_table_entries,
                  p->pt_table_indexing_policy, p->pt_table_replacement_policy,
                  PrefetchTableEntry(p->num_stream_counter_bits,
                                     p->num_indirect_counter_bits)),
    ipd(p->ipd_table_assoc, p->ipd_table_entries, p->ipd_table_indexing_policy,
        p->ipd_table_replacement_policy,
        IndirectPatternDetectorEntry(p->addr_array_len, shiftValues.size())),
    ipdEntryTrackingMisses(nullptr),
#if THE_ISA != NULL_ISA
    byteOrder(TheISA::GuestByteOrder)
#else
    byteOrder((ByteOrder) -1)
#endif
{
    fatal_if(byteOrder == static_cast<ByteOrder>(-1),
            "This prefetcher requires a defined ISA\n");
}

void
IndirectMemory::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses)
{
    // This prefetcher requires a PC
    if (!pfi.hasPC()) {
        return;
    }

    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();

    checkAccessMatchOnActiveEntries(pc, addr);

    // First check if this is a miss, if the prefetcher is tracking misses
    if (ipdEntryTrackingMisses != nullptr && miss) {
        // Check if the entry tracking misses has already set its second index
        if (!ipdEntryTrackingMisses->secondIndexSet) {
            trackMissIndex1(addr);
        } else {
            trackMissIndex2(addr);
        }
    } else {
        // if misses are not being tracked, attempt to detect stream accesses
        PrefetchTableEntry *pt_entry =
            prefetchTable.findEntry(pc, false /* unused */);
        if (pt_entry != nullptr) {
            prefetchTable.accessEntry(pt_entry);
            ptHits++;
            if (miss) {
                ptHitsWithCacheMiss++;
            }

            if (pt_entry->address != addr) {
                int new_stride = addr - pt_entry->address;
                bool stride_match = (new_stride == pt_entry->stride);
                if (stride_match && new_stride != 0) {
                    // Streaming access found
                    pt_entry->streamCounter++;
                } else {
                    pt_entry->streamCounter--;
                    if (pt_entry->streamCounter < streamCounterThreshold) {
                        pt_entry->stride = new_stride;
                    }
                }
                if (pt_entry->streamCounter >= streamCounterThreshold) {
                    for (unsigned int i = 1; i <= streamingDistance; i += 1) {
                        // Round strides up to atleast 1 cacheline
                        int pf_stride = pt_entry->stride;
                        if (abs(pf_stride) < blkSize) {
                            pf_stride = (pf_stride < 0) ? -blkSize : blkSize;
                        }

                        Addr new_addr = addr + i * pf_stride;
                        addresses.push_back(AddrPriority(new_addr, 0));
                        streamPfPushed++;
                    }
                }
                DPRINTF(IMPrefetcher,
                    "pc %#x addr %#x (%s) stride %d new_stride %d conf %d.\n",
                    pc, addr, miss ? "miss" : "hit", pt_entry->stride, new_stride,
                    static_cast<int>(pt_entry->streamCounter));
                pt_entry->address = addr;
                pt_entry->secure = is_secure;


                // if this is a read, read the data from the cache and assume
                // it is an index (this is only possible if the data is already
                // in the cache), also, only indexes up to 8 bytes are
                // considered

                if (!miss && !pfi.isWrite() && pfi.getSize() <= 8) {
                    int64_t index = 0;
                    bool read_index = true;
                    switch(pfi.getSize()) {
                        case sizeof(uint8_t):
                            index = pfi.get<uint8_t>(byteOrder);
                            break;
                        case sizeof(uint16_t):
                            index = pfi.get<uint16_t>(byteOrder);
                            break;
                        case sizeof(uint32_t):
                            index = pfi.get<uint32_t>(byteOrder);
                            break;
                        case sizeof(uint64_t):
                            index = pfi.get<uint64_t>(byteOrder);
                            break;
                        default:
                            // Ignore non-power-of-two sizes
                            read_index = false;
                    }
                    if (read_index && !pt_entry->enabled) {
                        // Not enabled (no pattern detected in this stream),
                        // add or update an entry in the pattern detector and
                        // start tracking misses
                        allocateOrUpdateIPDEntry(pt_entry, pc, index);
                    } else if (read_index) {
                        // Enabled entry, update the index
                        insertIndex(index, *pt_entry);

                        DPRINTF(IMPrefetcher,
                            "Reset idx: pc %#x (%#x + %d << %d = %#x) conf %d.\n",
                            pc, pt_entry->baseAddr, index, pt_entry->shift,
                            pt_entry->baseAddr + (index << pt_entry->shift),
                            static_cast<int>(pt_entry->indirectCounter));

                        // If the counter is high enough, start prefetching
                        if (pt_entry->indirectCounter > prefetchThreshold) {
                            // This does not make sense to generate multiple
                            // indirect prefetch request for a single indes.
                            Addr pf_addr = pt_entry->baseAddr +
                                (index << pt_entry->shift);
                            addresses.push_back(AddrPriority(pf_addr, 0));
                            indirectPfPushed++;
                        }
                    }
                }
            }
        } else {
            pt_entry = prefetchTable.findVictim(pc);
            assert(pt_entry != nullptr);
            prefetchTable.insertEntry(pc, false /* unused */, pt_entry);
            pt_entry->address = addr;
            pt_entry->secure = is_secure;
            ptAllocations++;
        }
    }
}

void
IndirectMemory::allocateOrUpdateIPDEntry(
    const PrefetchTableEntry *pt_entry, Addr stream_pc, int64_t index)
{
    // The address of the pt_entry is used to index the IPD
    Addr ipd_entry_addr = (Addr) pt_entry;
    IndirectPatternDetectorEntry *ipd_entry = ipd.findEntry(ipd_entry_addr,
                                                            false/* unused */);
    if (ipd_entry != nullptr) {
        ipd.accessEntry(ipd_entry);
        if (!ipd_entry->secondIndexSet) {
            // Second time we see an index, fill idx2
            ipd_entry->idx2 = index;
            ipd_entry->secondIndexSet = true;
            ipdEntryTrackingMisses = ipd_entry;
            ipdSecondAccess++;
            DPRINTF(IMPrefetcher, "IPD 2nd: pc %#x idx2 %d.\n", stream_pc, index);
        } else {
            // Third access! no pattern has been found so far,
            // release the IPD entry
            ipd.invalidate(ipd_entry);
            ipdEntryTrackingMisses = nullptr;
            ipdThirdAccessNoPattern++;
            DPRINTF(IMPrefetcher, "IPD 3rd: pc %#x released.\n", stream_pc);
        }
    } else {
        ipd_entry = ipd.findVictim(ipd_entry_addr);
        assert(ipd_entry != nullptr);
        ipd.insertEntry(ipd_entry_addr, false /* unused */, ipd_entry);
        ipd_entry->idx1 = index;
        ipdEntryTrackingMisses = ipd_entry;
        ipdAllocations++;
        DPRINTF(IMPrefetcher, "IPD 1st: pc %#x idx1 %d.\n", stream_pc, index);
    }
}

void
IndirectMemory::trackMissIndex1(Addr miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // If the second index is not set, we are just filling the baseAddr
    // vector
    assert(entry->numMisses < entry->baseAddr.size());
    std::vector<Addr> &ba_array = entry->baseAddr[entry->numMisses];
    int idx = 0;
    ipdTrackMiss1++;
    for (int shift : shiftValues) {
        ba_array[idx] = miss_addr - (entry->idx1 << shift);
        idx += 1;
    }
    entry->numMisses += 1;
    if (entry->numMisses == entry->baseAddr.size()) {
        // stop tracking misses once we have tracked enough
        ipdEntryTrackingMisses = nullptr;
    }
}
void
IndirectMemory::trackMissIndex2(Addr miss_addr)
{
    IndirectPatternDetectorEntry *entry = ipdEntryTrackingMisses;
    // Second index is filled, compare the addresses generated during
    // the previous misses (using idx1) against newly generated values
    // using idx2, if a match is found, fill the additional fields
    // of the PT entry
    ipdTrackMiss2++;
    for (int midx = 0; midx < entry->numMisses; midx += 1)
    {
        std::vector<Addr> &ba_array = entry->baseAddr[midx];
        int idx = 0;
        for (int shift : shiftValues) {
            if (ba_array[idx] == (miss_addr - (entry->idx2 << shift))) {
                // Match found!
                // Fill the corresponding pt_entry
                PrefetchTableEntry *pt_entry =
                    (PrefetchTableEntry *) entry->getTag();
                pt_entry->baseAddr = ba_array[idx];
                pt_entry->shift = shift;
                pt_entry->enabled = true;
                pt_entry->indirectCounter.reset();
                // Release the current IPD Entry
                ipd.invalidate(entry);
                // Do not track more misses
                ipdEntryTrackingMisses = nullptr;
                ipdFindPattern++;
                return;
            }
            idx += 1;
        }
    }
}

void
IndirectMemory::checkAccessMatchOnActiveEntries(Addr pc, Addr addr)
{
    for (auto &pt_entry : prefetchTable) {
        // We should not try to check for the same pc.
        if (pt_entry.enabled && pt_entry.getTag() != pc) {
            checkAccessMatchOnIndex(pc, addr, pt_entry);
        }
    }
}

void
IndirectMemory::regStats()
{
    Queued::regStats();

    ptAllocations
        .name(name() + ".imp_num_pt_alloc")
        .desc("number of IMP prefetch table allocation")
        ;
    ptHits
        .name(name() + ".imp_num_pt_hit")
        .desc("number of IMP prefetch table hit")
        ;
    ptHitsWithCacheMiss
        .name(name() + ".imp_num_pt_hit_with_cache_miss")
        .desc("number of IMP prefetch table hit with cache miss")
        ;
    streamPfPushed 
        .name(name() + ".imp_num_stream_pf_pushed")
        .desc("number of IMP stream prefetch pushed")
        ;
    ipdAllocations
        .name(name() + ".imp_num_ipd_alloc")
        .desc("number of IMP IPD allocation (First Idx)")
        ;
    ipdSecondAccess
        .name(name() + ".imp_num_ipd_second_idx")
        .desc("number of IMP IPD second access (Second Idx)")
        ;
    ipdThirdAccessNoPattern
        .name(name() + ".imp_num_ipd_third_idx_no_pattern")
        .desc("number of IMP IPD third access (Third Idx No Pattern)")
        ;
    ipdTrackMiss1
        .name(name() + ".imp_num_ipd_track_miss1")
        .desc("number of IMP IPD track miss 1")
        ;
    ipdTrackMiss2
        .name(name() + ".imp_num_ipd_track_miss2")
        .desc("number of IMP IPD track miss 2")
        ;
    ipdFindPattern
        .name(name() + ".imp_num_ipd_find_pattern")
        .desc("number of IMP IPD recognizes a pattern")
        ;
    indirectPfPushed 
        .name(name() + ".imp_num_indirect_pf_pushed")
        .desc("number of IMP indirect prefetch pushed")
        ;
}

} // namespace Prefetcher

Prefetcher::IndirectMemory*
IndirectMemoryPrefetcherParams::create()
{
    return new Prefetcher::IndirectMemory(this);
}
