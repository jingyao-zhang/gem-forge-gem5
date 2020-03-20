/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
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
 *
 * Authors: Gabe Black
 */

#ifndef __ARCH_X86_TLB_HH__
#define __ARCH_X86_TLB_HH__

#include <list>
#include <vector>

#include "arch/generic/se_page_walker.hh"
#include "arch/generic/tlb.hh"
#include "arch/x86/pagetable.hh"
#include "arch/x86/tlb_cache.hh"
#include "base/trie.hh"
#include "mem/request.hh"
#include "params/X86TLB.hh"

#include <memory>

class ThreadContext;

namespace X86ISA
{
    class Walker;

    class TLB : public BaseTLB
    {
      protected:
        friend class Walker;

        typedef std::list<TlbEntry *> EntryList;

        uint32_t configAddress;

      public:

        typedef X86TLBParams Params;
        TLB(const Params *p);

        void takeOverFrom(BaseTLB *otlb) override {}

        TlbEntry *lookupL1(Addr va, bool isLastLevel,
            bool updateStats, bool updateLRU);
        TlbEntry *lookupL2(Addr va, bool isLastLevel,
            bool updateStats, bool updateLRU);
        TlbEntry *lookup(Addr va, bool isLastLevel,
            bool updateStats, bool updateLRU, int &hitLevel);

        void setConfigAddress(uint32_t addr);

      protected:

        Walker * walker;

      public:
        Walker *getWalker();

        void flushAll() override;

        void flushNonGlobal();

        void demapPage(Addr va, uint64_t asn) override;

      protected:
        uint32_t size;
        uint32_t assoc;
        uint32_t l2size;
        uint32_t l2assoc;
        Cycles l2HitLatency;
        // A fixed walker latency for SE mode.
        Cycles walkerSELatency;
        // Number of simultaneous page walking for SE mode.
        uint32_t walkerSEPort;
        // Whether we enable TLB timing in SE mode.
        bool timingSE;

        /**
         * Legacy fully-associative L1 TLB. I keep it here
         * cause the new multi-level TLB has assumed fixed page size.
         */
        TLBSet tlbCache;

        /**
         * New implementation of 2-level, set-partitioned TLB.
         */
        std::unique_ptr<TLBCache> l1tlb;
        std::unique_ptr<TLBCache> l2tlb;
        std::unique_ptr<SEPageWalker> sePageWalker;

        // Statistics
        Stats::Scalar rdAccesses;
        Stats::Scalar wrAccesses;
        Stats::Scalar rdMisses;
        Stats::Scalar wrMisses;
        Stats::Scalar l1Accesses;
        Stats::Scalar l1Misses;
        Stats::Scalar l2Accesses;
        Stats::Scalar l2Misses;

        Fault translateInt(const RequestPtr &req, ThreadContext *tc);

        /**
         * Perform the translation and serve the TLB miss.
         * @param updateStats: whether this translation should be counted.
         */
        Fault translate(const RequestPtr &req, ThreadContext *tc,
                Translation *translation, Mode mode,
                bool &delayedResponse, Cycles &delayedResponseCycles,
                bool timing, bool isLastLeve, bool updateStats);

        void translateTimingImpl(
            const RequestPtr &req, ThreadContext *tc,
            Translation *translation, Mode mode, bool isLastLevel);

      public:

        Fault translateAtomic(
            const RequestPtr &req, ThreadContext *tc, Mode mode) override;
        void translateTiming(
            const RequestPtr &req, ThreadContext *tc,
            Translation *translation, Mode mode) override {
            translateTimingImpl(req, tc, translation, mode, false);
        }
        void translateTimingAtLastLevel(
            const RequestPtr &req, ThreadContext *tc,
            Translation *translation, Mode mode) {
            translateTimingImpl(req, tc, translation, mode, true);
        }

        /**
         * Do post-translation physical address finalization.
         *
         * Some addresses, for example requests going to the APIC,
         * need post-translation updates. Such physical addresses are
         * remapped into a "magic" part of the physical address space
         * by this method.
         *
         * @param req Request to updated in-place.
         * @param tc Thread context that created the request.
         * @param mode Request type (read/write/execute).
         * @return A fault on failure, NoFault otherwise.
         */
        Fault finalizePhysical(const RequestPtr &req, ThreadContext *tc,
                               Mode mode) const override;

        TlbEntry *insert(Addr vpn, const TlbEntry &entry,
            bool isLastLevel);

        /*
         * Function to register Stats
         */
        void regStats() override;

        // Checkpointing
        void serialize(CheckpointOut &cp) const override;
        void unserialize(CheckpointIn &cp) override;

        /**
         * Get the table walker port. This is used for
         * migrating port connections during a CPU takeOverFrom()
         * call. For architectures that do not have a table walker,
         * NULL is returned, hence the use of a pointer rather than a
         * reference. For X86 this method will always return a valid
         * port pointer.
         *
         * @return A pointer to the walker port
         */
        Port *getTableWalkerPort() override;

        /**
         * Schedule an event to mark Translation finish in SE mode.
         */
        struct DelayedTranslationEvent : public Event {
        public:
          TLB *tlb;
          bool isLastLevel;
          ThreadContext *tc;
          Translation *translation;
          RequestPtr req;
          BaseTLB::Mode mode;
          Fault fault;
          std::string n;
          DelayedTranslationEvent(
            TLB *_tlb, bool _isLastLevel, ThreadContext *_tc,
            Translation *_translation, const RequestPtr &_req,
            BaseTLB::Mode _mode, Fault _fault);
          void process() override;
          const char *description() const { return "DelayedTranslationEvent"; }
          const std::string name() const { return this->n; }
        };
    };
}

#endif // __ARCH_X86_TLB_HH__
