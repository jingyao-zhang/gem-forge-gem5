
from m5.objects import *

def initializePrefetch(options, system):
    # Only works in classical memory system.
    if options.ruby:
        return
    if options.llvm_prefetch == 0:
        return
    for cpu in system.cpu:
        cpu.dcache.prefetch_on_access = options.gem_forge_prefetch_on_access
        if options.gem_forge_prefetcher == 'imp':
            cpu.dcache.prefetcher = IndirectMemoryPrefetcher(
                streaming_distance=8,
                use_virtual_addresses=True,
                index_queue_size=16,
            )
            if not cpu.dcache.prefetch_on_access:
                raise ValueError('IMP must be used with PrefetchOnAccess.')
            if not hasattr(cpu, 'dtb'):
                raise ValueError('IMP requires TLB to work with virtual address.')
            cpu.dcache.prefetcher.registerTLB(cpu.dtb)
        else:
            cpu.dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
        if options.l1_5dcache:
            cpu.l1_5dcache.prefetch_on_access = options.gem_forge_prefetch_on_access
            if options.gem_forge_prefetcher == 'isb':
                cpu.l1_5dcache.prefetcher = IrregularStreamBufferPrefetcher(
                    degree=8,
                    # address_map_cache_assoc=8,
                    address_map_cache_entries="65536",
                    # training_unit_assoc=8,
                    training_unit_entries="65536",
                )
            else:
                cpu.l1_5dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
    system.l2.prefetch_on_access = options.gem_forge_prefetch_on_access
    if options.gem_forge_prefetcher == 'isb':
        # ISB should work at LLC.
        system.l2.prefetcher = IrregularStreamBufferPrefetcher(
            degree=8,
            # address_map_cache_assoc=8,
            address_map_cache_entries="65536",
            # training_unit_assoc=8,
            training_unit_entries="65536",
        )
    else:
        system.l2.prefetcher = StridePrefetcher(degree=8, latency=1)