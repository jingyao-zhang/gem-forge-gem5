/*
 * Copyright (c) 2007-2008 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "arch/x86/tlb.hh"

#include <cstring>
#include <memory>

#include "arch/x86/faults.hh"
#include "arch/x86/insts/microldstop.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/pseudo_inst_abi.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/regs/msr.hh"
#include "arch/x86/x86_traits.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/TLB.hh"
#include "mem/packet_access.hh"
#include "mem/page_table.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/pseudo_inst.hh"

namespace X86ISA {

TLB::TLB(const Params *p)
    : BaseTLB(p), configAddress(0),
    size(p->size), assoc(p->assoc),
    l2size(p->l2size), l2assoc(p->l2assoc), l2HitLatency(p->l2_lat),
    walkerSELatency(p->walker_se_lat), walkerSEPort(p->walker_se_port),
    timingSE(p->timing_se),
    tlbCache(p->size, 0),
    m5opRange(p->system->m5opRange())
{
    if (!size)
        fatal("TLBs must have a non-zero size.\n");

    walker = p->walker;
    walker->setTLB(this);

    /**
     * Check if we are using new multi-level set-partitioned
     * TLB implementation.
     */
    if (this->assoc > 0) {
        // This is not a fully assoicative TLB, we use TLBCache.
        this->l1tlb = m5::make_unique<TLBCache>(this->size, this->assoc);
    }
    if (this->l2size > 0) {
        // Set up the L2 TLB.
        this->l2tlb = m5::make_unique<TLBCache>(this->l2size, this->l2assoc);
    }
    /**
     * Check if we want to have timing in se.
     */
    if (this->timingSE) {
        this->sePageWalker = m5::make_unique<SEPageWalker>(
            this->name() + ".se_walker",
            this->walkerSELatency, this->walkerSEPort);
    }
}

TlbEntry *
TLB::insert(Addr vpn, const TlbEntry &entry, bool isLastLevel)
{
    /**
     * Normally we insert into both level TLB, unless it is
     * explicitly marked for the LastLevel only.
     */
    if (this->l2tlb) {
        auto newEntry = this->l2tlb->insert(vpn, entry);
        if (isLastLevel) {
            return newEntry;
        }
    }
    if (this->l1tlb) {
        return this->l1tlb->insert(vpn, entry);
    }
    // Legacy implementation.
    return this->tlbCache.insert(vpn, entry);
}

TlbEntry *
TLB::lookupL1(Addr va, bool isLastLevel, bool updateStats,
    bool updateLRU) {
    if (isLastLevel && this->l2tlb) {
        // Goes directly to L2TLB.
        return nullptr;
    }
    TlbEntry *entry = nullptr;
    if (this->l1tlb) {
        entry = this->l1tlb->lookup(va, updateLRU);
    } else {
        entry = this->tlbCache.lookup(va, updateLRU);
    }
    if (updateStats) {
        this->l1Accesses++;
        if (!entry) this->l1Misses++;
    }
    return entry;
}

TlbEntry *
TLB::lookupL2(Addr va, bool isLastLevel, bool updateStats,
    bool updateLRU) {
    TlbEntry *entry = nullptr;
    if (this->l2tlb) {
        entry = this->l1tlb->lookup(va, updateLRU);
        if (updateStats) {
            this->l2Accesses++;
            if (!entry) this->l2Misses++;
        }
    }
    return entry;
}

TlbEntry *
TLB::lookup(Addr va, bool isLastLevel, bool updateStats,
    bool updateLRU, int &hitLevel)
{
    hitLevel = 0;
    TlbEntry *entry = this->lookupL1(
        va, isLastLevel, updateStats, updateLRU);
    // Look up in L2 if we miss in L1.
    if (!entry) {
        hitLevel++;
        entry = this->lookupL2(
            va, isLastLevel, updateStats, updateLRU);
        if (!entry) {
            // Miss in L2.
            hitLevel++;
        } else {
            /**
             * Hit in L2. Bring in the entry to L1 if this
             * access is not limited to LastLevel only.
             */
            if (!isLastLevel) {
                if (this->l1tlb) {
                    this->l1tlb->insert(entry->vaddr, *entry);
                } else {
                    this->tlbCache.insert(entry->vaddr, *entry);
                }
            }
        }
    }
    return entry;
}

void
TLB::flushAll()
{
    DPRINTF(TLB, "Invalidating all entries.\n");
    if (this->l2tlb) {
        this->l2tlb->flushAll();
    }
    if (this->l1tlb) {
        this->l1tlb->flushAll();
        return;
    }
    this->tlbCache.flushAll();
}

void
TLB::setConfigAddress(uint32_t addr)
{
    configAddress = addr;
}

void
TLB::flushNonGlobal()
{
    DPRINTF(TLB, "Invalidating all non global entries.\n");
    if (this->l2tlb) {
        this->l2tlb->flushNonGlobal();
    }
    if (this->l1tlb) {
        this->l1tlb->flushNonGlobal();
        return;
    }
    this->tlbCache.flushNonGlobal();
}

void
TLB::demapPage(Addr va, uint64_t asn)
{
    if (this->l2tlb) {
        this->l2tlb->demapPage(va, asn);
    }
    if (this->l1tlb) {
        this->l1tlb->demapPage(va, asn);
        return;
    }
    this->tlbCache.demapPage(va, asn);
}

namespace
{

Cycles
localMiscRegAccess(bool read, MiscRegIndex regNum,
                   ThreadContext *tc, PacketPtr pkt)
{
    if (read) {
        RegVal data = htole(tc->readMiscReg(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->setData((uint8_t *)&data);
    } else {
        RegVal data = htole(tc->readMiscRegNoEffect(regNum));
        assert(pkt->getSize() <= sizeof(RegVal));
        pkt->writeData((uint8_t *)&data);
        tc->setMiscReg(regNum, letoh(data));
    }
    return Cycles(1);
}

} // anonymous namespace

Fault
TLB::translateInt(bool read, RequestPtr req, ThreadContext *tc)
{
    DPRINTF(TLB, "Addresses references internal memory.\n");
    Addr vaddr = req->getVaddr();
    Addr prefix = (vaddr >> 3) & IntAddrPrefixMask;
    if (prefix == IntAddrPrefixCPUID) {
        panic("CPUID memory space not yet implemented!\n");
    } else if (prefix == IntAddrPrefixMSR) {
        vaddr = (vaddr >> 3) & ~IntAddrPrefixMask;

        MiscRegIndex regNum;
        if (!msrAddrToIndex(regNum, vaddr))
            return std::make_shared<GeneralProtection>(0);

        req->setPaddr(req->getVaddr());
        req->setLocalAccessor(
            [read,regNum](ThreadContext *tc, PacketPtr pkt)
            {
                return localMiscRegAccess(read, regNum, tc, pkt);
            }
        );

        return NoFault;
    } else if (prefix == IntAddrPrefixIO) {
        // TODO If CPL > IOPL or in virtual mode, check the I/O permission
        // bitmap in the TSS.

        Addr IOPort = vaddr & ~IntAddrPrefixMask;
        // Make sure the address fits in the expected 16 bit IO address
        // space.
        assert(!(IOPort & ~0xFFFF));
        if (IOPort == 0xCF8 && req->getSize() == 4) {
            req->setPaddr(req->getVaddr());
            req->setLocalAccessor(
                [read](ThreadContext *tc, PacketPtr pkt)
                {
                    return localMiscRegAccess(
                            read, MISCREG_PCI_CONFIG_ADDRESS, tc, pkt);
                }
            );
        } else if ((IOPort & ~mask(2)) == 0xCFC) {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            Addr configAddress =
                tc->readMiscRegNoEffect(MISCREG_PCI_CONFIG_ADDRESS);
            if (bits(configAddress, 31, 31)) {
                req->setPaddr(PhysAddrPrefixPciConfig |
                        mbits(configAddress, 30, 2) |
                        (IOPort & mask(2)));
            } else {
                req->setPaddr(PhysAddrPrefixIO | IOPort);
            }
        } else {
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(PhysAddrPrefixIO | IOPort);
        }
        return NoFault;
    } else {
        panic("Access to unrecognized internal address space %#x.\n",
                prefix);
    }
}

Fault
TLB::finalizePhysical(const RequestPtr &req,
                      ThreadContext *tc, Mode mode) const
{
    Addr paddr = req->getPaddr();

    if (m5opRange.contains(paddr)) {
        req->setFlags(Request::STRICT_ORDER);
        uint8_t func;
        PseudoInst::decodeAddrOffset(paddr - m5opRange.start(), func);
        req->setLocalAccessor(
            [func, mode](ThreadContext *tc, PacketPtr pkt) -> Cycles
            {
                uint64_t ret;
                PseudoInst::pseudoInst<X86PseudoInstABI>(tc, func, ret);
                if (mode == Read)
                    pkt->setLE(ret);
                return Cycles(1);
            }
        );
    } else if (FullSystem) {
        // Check for an access to the local APIC
        LocalApicBase localApicBase =
            tc->readMiscRegNoEffect(MISCREG_APIC_BASE);
        AddrRange apicRange(localApicBase.base * PageBytes,
                            (localApicBase.base + 1) * PageBytes);

        if (apicRange.contains(paddr)) {
            // The Intel developer's manuals say the below restrictions apply,
            // but the linux kernel, because of a compiler optimization, breaks
            // them.
            /*
            // Check alignment
            if (paddr & ((32/8) - 1))
                return new GeneralProtection(0);
            // Check access size
            if (req->getSize() != (32/8))
                return new GeneralProtection(0);
            */
            // Force the access to be uncacheable.
            req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
            req->setPaddr(x86LocalAPICAddress(tc->contextId(),
                                              paddr - apicRange.start()));
        }
    }

    return NoFault;
}

Fault
TLB::translate(const RequestPtr &req,
        ThreadContext *tc, Translation *translation,
        Mode mode, bool &delayedResponse, Cycles &delayedResponseCycles,
        bool timing, bool isLastLevel, bool updateStats)
{
    Request::Flags flags = req->getFlags();
    int seg = flags & SegmentFlagMask;
    bool storeCheck = flags & (StoreCheck << FlagShift);

    delayedResponse = false;
    delayedResponseCycles = Cycles(0);

    // If this is true, we're dealing with a request to a non-memory address
    // space.
    if (seg == SEGMENT_REG_MS) {
        return translateInt(mode == Read, req, tc);
    }

    Addr vaddr = req->getVaddr();
    DPRINTF(TLB, "Translating %#x, LastLevel %d.\n", vaddr, isLastLevel);

    HandyM5Reg m5Reg = tc->readMiscRegNoEffect(MISCREG_M5_REG);

    // If protected mode has been enabled...
    if (m5Reg.prot) {
        DPRINTF(TLB, "In protected mode.\n");
        // If we're not in 64-bit mode, do protection/limit checks
        if (m5Reg.mode != LongMode) {
            DPRINTF(TLB, "Not in long mode. Checking segment protection.\n");
            // Check for a NULL segment selector.
            if (!(seg == SEGMENT_REG_TSG || seg == SYS_SEGMENT_REG_IDTR ||
                        seg == SEGMENT_REG_HS || seg == SEGMENT_REG_LS)
                    && !tc->readMiscRegNoEffect(MISCREG_SEG_SEL(seg)))
                return std::make_shared<GeneralProtection>(0);
            bool expandDown = false;
            SegAttr attr = tc->readMiscRegNoEffect(MISCREG_SEG_ATTR(seg));
            if (seg >= SEGMENT_REG_ES && seg <= SEGMENT_REG_HS) {
                if (!attr.writable && (mode == Write || storeCheck))
                    return std::make_shared<GeneralProtection>(0);
                if (!attr.readable && mode == Read)
                    return std::make_shared<GeneralProtection>(0);
                expandDown = attr.expandDown;

            }
            Addr base = tc->readMiscRegNoEffect(MISCREG_SEG_BASE(seg));
            Addr limit = tc->readMiscRegNoEffect(MISCREG_SEG_LIMIT(seg));
            bool sizeOverride = (flags & (AddrSizeFlagBit << FlagShift));
            unsigned logSize = sizeOverride ? (unsigned)m5Reg.altAddr
                                            : (unsigned)m5Reg.defAddr;
            int size = (1 << logSize) * 8;
            Addr offset = bits(vaddr - base, size - 1, 0);
            Addr endOffset = offset + req->getSize() - 1;
            if (expandDown) {
                DPRINTF(TLB, "Checking an expand down segment.\n");
                warn_once("Expand down segments are untested.\n");
                if (offset <= limit || endOffset <= limit)
                    return std::make_shared<GeneralProtection>(0);
            } else {
                if (offset > limit || endOffset > limit)
                    return std::make_shared<GeneralProtection>(0);
            }
        }
        if (m5Reg.submode != SixtyFourBitMode ||
                (flags & (AddrSizeFlagBit << FlagShift)))
            vaddr &= mask(32);
        // If paging is enabled, do the translation.
        if (m5Reg.paging) {
            DPRINTF(TLB, "Paging enabled.\n");
            // The vaddr already has the segment base applied.
            int hitLevel = 0;
            TlbEntry *entry = lookup(vaddr, isLastLevel,
                updateStats, true /* UpdateLRU */, hitLevel);
            if (updateStats) {
                if (mode == Read) rdAccesses++;
                else wrAccesses++;
            }
            if (!entry) {
                DPRINTF(TLB, "Handling a TLB miss for address %#x at pc %#x.\n",
                        vaddr, tc->instAddr());
                if (updateStats) {
                    if (mode == Read) rdMisses++;
                    else wrMisses++;
                }
                if (FullSystem) {
                    Fault fault = walker->start(tc, translation, req, mode);
                    if (timing || fault != NoFault) {
                        // This gets ignored in atomic mode.
                        delayedResponse = true;
                        return fault;
                    }
                    int hitLevel = 0;
                    entry = lookup(vaddr, isLastLevel,
                        false /* updateStats */, true /* updateLRU */, hitLevel);
                    assert(entry);
                } else {
                    Process *p = tc->getProcessPtr();
                    const EmulationPageTable::Entry *pte =
                        p->pTable->lookup(vaddr);
                    if (!pte) {
                        return std::make_shared<PageFault>(vaddr, true, mode,
                                                           true, false);
                    } else {
                        Addr alignedVaddr = p->pTable->pageAlign(vaddr);
                        DPRINTF(TLB, "Mapping %#x to %#x\n", alignedVaddr,
                                pte->paddr);
                        entry = insert(alignedVaddr, TlbEntry(
                            p->pTable->pid(), alignedVaddr, pte->paddr,
                            pte->flags & EmulationPageTable::Uncacheable,
                            pte->flags & EmulationPageTable::ReadOnly),
                            isLastLevel);
                    }
                    DPRINTF(TLB, "HitLevel %d (Miss) was serviced.\n",
                        hitLevel);
                }
            }
            if (!FullSystem && timing && this->timingSE && hitLevel > 0) {
                // We will delay the response and schedule a event.
                delayedResponse = true;
                Addr pageVAddr = tc->getProcessPtr()->pTable->pageAlign(vaddr);
                switch (hitLevel) {
                case 2:
                    // This access goes to page walker.
                    delayedResponseCycles = this->sePageWalker->walk(
                        pageVAddr, this->walker->curCycle());
                    break;
                case 1:
                    assert(this->l2tlb && "Missing L2TLB.");
                    delayedResponseCycles = this->l2HitLatency;
                    break;
                default: panic("Illegal TLB HitLevel %d.\n", hitLevel);
                }
                DPRINTF(TLB, "SETiming HitLevel %d, Delayed by %s\n",
                    hitLevel, delayedResponseCycles);
            }

            DPRINTF(TLB, "Entry found with paddr %#x, "
                    "doing protection checks.\n", entry->paddr);
            // Do paging protection checks.
            bool inUser = (m5Reg.cpl == 3 &&
                    !(flags & (CPL0FlagBit << FlagShift)));
            CR0 cr0 = tc->readMiscRegNoEffect(MISCREG_CR0);
            bool badWrite = (!entry->writable && (inUser || cr0.wp));
            if ((inUser && !entry->user) || (mode == Write && badWrite)) {
                // The page must have been present to get into the TLB in
                // the first place. We'll assume the reserved bits are
                // fine even though we're not checking them.
                return std::make_shared<PageFault>(vaddr, true, mode, inUser,
                                                   false);
            }
            if (storeCheck && badWrite) {
                // This would fault if this were a write, so return a page
                // fault that reflects that happening.
                return std::make_shared<PageFault>(vaddr, true, Write, inUser,
                                                   false);
            }

            Addr paddr = entry->paddr | (vaddr & mask(entry->logBytes));
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, paddr);
            req->setPaddr(paddr);
            if (entry->uncacheable)
                req->setFlags(Request::UNCACHEABLE | Request::STRICT_ORDER);
        } else {
            //Use the address which already has segmentation applied.
            DPRINTF(TLB, "Paging disabled.\n");
            DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
            req->setPaddr(vaddr);
        }
    } else {
        // Real mode
        DPRINTF(TLB, "In real mode.\n");
        DPRINTF(TLB, "Translated %#x -> %#x.\n", vaddr, vaddr);
        req->setPaddr(vaddr);
    }

    return finalizePhysical(req, tc, mode);
}

Fault
TLB::translateAtomic(const RequestPtr &req, ThreadContext *tc, Mode mode)
{
    bool delayedResponse = false;
    Cycles delayedResponseCycles = Cycles(0);
    bool updateStats = true;
    return TLB::translate(req, tc, NULL, mode, delayedResponse,
        delayedResponseCycles, false, false, updateStats);
}

Fault
TLB::translateFunctional(const RequestPtr &req, ThreadContext *tc, Mode mode)
{
    unsigned logBytes;
    const Addr vaddr = req->getVaddr();
    Addr addr = vaddr;
    Addr paddr = 0;
    if (FullSystem) {
        Fault fault = walker->startFunctional(tc, addr, logBytes, mode);
        if (fault != NoFault)
            return fault;
        paddr = insertBits(addr, logBytes - 1, 0, vaddr);
    } else {
        Process *process = tc->getProcessPtr();
        const auto *pte = process->pTable->lookup(vaddr);

        if (!pte && mode != Execute) {
            // Check if we just need to grow the stack.
            if (process->fixupFault(vaddr)) {
                // If we did, lookup the entry for the new page.
                pte = process->pTable->lookup(vaddr);
            }
        }

        if (!pte)
            return std::make_shared<PageFault>(vaddr, true, mode, true, false);

        paddr = pte->paddr | process->pTable->pageOffset(vaddr);
    }
    DPRINTF(TLB, "Translated (functional) %#x -> %#x.\n", vaddr, paddr);
    req->setPaddr(paddr);
    return NoFault;
}

void
TLB::translateTimingImpl(const RequestPtr &req, ThreadContext *tc,
        Translation *translation, Mode mode, bool isLastLevel)
{
    bool delayedResponse;
    Cycles delayedResponseCycles = Cycles(0);
    bool updateStats = true;
    assert(translation);
    Fault fault = TLB::translate(req, tc, translation, mode, delayedResponse,
            delayedResponseCycles, true, isLastLevel, updateStats);
    if (!delayedResponse) {
        translation->finish(fault, req, tc, mode);
    } else {
        translation->markDelayed();
        if (delayedResponseCycles > 0) {
            // We know the delayed cycles, schedule a event.
            DPRINTF(TLB, "Translation %#x LastLevel %d Delayed by %s.\n",
                req->getVaddr(), isLastLevel, delayedResponseCycles);
            auto event = new DelayedTranslationEvent(
                this, isLastLevel, tc, translation, req, mode, fault);
            // We borrow the walker's cycle.
            this->schedule(event, this->walker->clockEdge(delayedResponseCycles));
        }
    }
}

Walker *
TLB::getWalker()
{
    return walker;
}

void
TLB::regStats()
{
    using namespace Stats;
    BaseTLB::regStats();
    rdAccesses
        .name(name() + ".rdAccesses")
        .desc("TLB accesses on read requests");

    wrAccesses
        .name(name() + ".wrAccesses")
        .desc("TLB accesses on write requests");

    rdMisses
        .name(name() + ".rdMisses")
        .desc("TLB misses on read requests");

    wrMisses
        .name(name() + ".wrMisses")
        .desc("TLB misses on write requests");

    l1Accesses
        .name(name() + ".l1Accesses")
        .desc("L1 TLB accesses");
    l1Misses
        .name(name() + ".l1Misses")
        .desc("L1 TLB misses");
    l2Accesses
        .name(name() + ".l2Accesses")
        .desc("L2 TLB accesses");
    l2Misses
        .name(name() + ".l2Misses")
        .desc("L2 TLB misses");

    if (this->sePageWalker) {
        this->sePageWalker->regStats();
    }
}

void
TLB::serialize(CheckpointOut &cp) const
{
    this->tlbCache.serializeSection(cp, "L1TLBSet");
    if (this->l1tlb) {
        this->l1tlb->serializeSection(cp, "L1TLB");
    }
    if (this->l2tlb) {
        this->l2tlb->serializeSection(cp, "L2TLB");
    }
}

void
TLB::unserialize(CheckpointIn &cp)
{
    // Do not allow to restore with a smaller tlb.
    this->tlbCache.unserializeSection(cp, "L1TLBSet");
    if (this->l1tlb) {
        this->l1tlb->unserializeSection(cp, "L1TLB");
    }
    if (this->l2tlb) {
        this->l2tlb->unserializeSection(cp, "L2TLB");
    }
}

Port *
TLB::getTableWalkerPort()
{
    return &walker->getPort("port");
}

TLB::DelayedTranslationEvent::DelayedTranslationEvent(
  TLB *_tlb, bool _isLastLevel, ThreadContext *_tc, Translation *_translation,
  const RequestPtr &_req, BaseTLB::Mode _mode, Fault _fault)
  : tlb(_tlb), isLastLevel(_isLastLevel), tc(_tc), translation(_translation),
    req(_req), mode(_mode), fault(_fault), n("DelayedTranslationEvent") {
  assert(this->fault == NoFault && "Fault on DelayedTranslation.");
  assert(req->hasPaddr() && "Req has no paddr at constructor.\n");
  this->setFlags(EventBase::AutoDelete);
}

void TLB::DelayedTranslationEvent:: process() {
  DPRINTF(TLB, "Serve DelayedTranslationEvent %#x, Squashed %d.\n",
    this->req->getVaddr(), this->translation->squashed());
  if (this->translation->squashed()) {
    // finish the translation which will delete the translation object
    this->translation->finish(
        std::make_shared<UnimpFault>("Squashed Inst"),
        this->req, this->tc, this->mode);
    return;
  }
  bool delayedResponse;
  Cycles delayedResponseCycles = Cycles(0);
  bool timing = false;
  bool updateStats = false;
  Fault fault = tlb->translate(
    req, tc, NULL, mode, delayedResponse, delayedResponseCycles,
    timing, this->isLastLevel, updateStats);
  if (!this->isLastLevel) {
    assert(!delayedResponse);
  }
  assert(req->hasPaddr() && "Req has no paddr.\n");
  this->translation->finish(fault, req, tc, mode);
}

} // namespace X86ISA

X86ISA::TLB *
X86TLBParams::create()
{
    return new X86ISA::TLB(this);
}
