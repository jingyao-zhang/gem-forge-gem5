/*
 * Copyright (c) 2012-2014 ARM Limited
 * All rights reserved
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

#include "cpu/minor/stats.hh"

#include "base/output.hh"
#include <cassert>

namespace Minor
{

MinorStats::MinorStats()
{ }

void MinorStats::updateLoadBlockedStat(Addr pc, int upc, uint64_t cycles)
{
    // Use (pc << 3) + upc as the key.
    if (upc > UPC_MASK) {
        panic("Overflow of upc at pc %#x, upc %d.\n", pc, upc);
    }
    assert(upc <= UPC_MASK && "Overflow of upc.");
    auto key = (pc << UPC_WIDTH) + upc;
    auto &stat = this->loadBlockedPCStat.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key), std::forward_as_tuple())
        .first->second;
    stat.times++;
    stat.cycles += cycles;
    this->loadBlockedIssueCycles += cycles;
    this->loadBlockedIssueInsts++;
}

void MinorStats::resetLoadBlockedStat()
{
    this->loadBlockedPCStat.clear();
}

void MinorStats::dumpLoadBlockedStat()
{
    std::string fn = std::to_string(this->cpuId);
    fn += ".";
    fn += std::to_string(this->dumped);
    fn += ".txt";
    auto outputStream = this->loadBlockedDir->findOrCreate(fn);
    auto &stream = *outputStream->stream();
    for (const auto &record : this->loadBlockedPCStat) {
        auto pc = record.first >> UPC_WIDTH;
        auto upc = record.first & UPC_MASK;
        auto cpi = static_cast<float>(record.second.cycles) /
            static_cast<float>(record.second.times);
        stream << std::hex << pc << ' ' << upc << ' ' << std::dec << cpi << ' '
            << record.second.times << '\n';
    }
    this->loadBlockedDir->close(outputStream);
    this->dumped++;
}

void
MinorStats::regStats(const std::string &name, BaseCPU &baseCpu)
{
    this->cpuId = baseCpu.cpuId();
    this->loadBlockedDir = simout.createSubdirectory("loadBlocked");

    Stats::registerDumpCallback(
        new MakeCallback<MinorStats, &MinorStats::dumpLoadBlockedStat>(
            this, true));
    Stats::registerResetCallback(
        new MakeCallback<MinorStats, &MinorStats::resetLoadBlockedStat>(
            this, true));

    numFetch2Branches
        .name(name + ".fetch2.branches")
        .desc("Number of branches fetched");

    numDecodedInsts
        .name(name + ".decode.insts")
        .desc("Number of instructions decoded");
    numDecodedOps
        .name(name + ".decode.ops")
        .desc("Number of ops (including micro ops) decoded");

    numLSQLoadOps
        .name(name + ".lsq.loads")
        .desc("Number of load ops executed");
    numLSQStoreOps
        .name(name + ".lsq.stores")
        .desc("Number of store ops executed");

    numIQIntReads
        .name(name + ".execute.iqIntReads")
        .desc("Number of reads to int iq");
    numIQIntWrites
        .name(name + ".execute.iqIntWrites")
        .desc("Number of writes to int iq");
    numIQIntWakeups
        .name(name + ".execute.iqIntWakeups")
        .desc("Number of wakeups to int iq");
    numIQFpReads
        .name(name + ".execute.iqFpReads")
        .desc("Number of reads to fp iq");
    numIQFpWrites
        .name(name + ".execute.iqFpWrites")
        .desc("Number of writes to fp iq");
    numIQFpWakeups
        .name(name + ".execute.iqFpWakeups")
        .desc("Number of wakeups to fp iq");

    numIntRegReads
        .name(name + ".execute.intRegReads")
        .desc("Number of reads to int regs");
    numIntRegWrites
        .name(name + ".execute.intRegWrites")
        .desc("Number of writes to int regs");
    numFpRegReads
        .name(name + ".execute.fpRegReads")
        .desc("Number of reads to fp regs");
    numFpRegWrites
        .name(name + ".execute.fpRegWrites")
        .desc("Number of writes to fp regs");

    numCommittedIntOps
        .name(name + ".commit.intOps")
        .desc("Number of int ops committed");
    numCommittedFpOps
        .name(name + ".commit.fpOps")
        .desc("Number of fp ops committed");
    numCommittedCallInsts
        .name(name + ".commit.callInsts")
        .desc("Number of call insts committed");

    numInsts
        .name(name + ".committedInsts")
        .desc("Number of instructions committed");

    numOps
        .name(name + ".committedOps")
        .desc("Number of ops (including micro ops) committed");

    numDiscardedOps
        .name(name + ".discardedOps")
        .desc("Number of ops (including micro ops) which were discarded "
            "before commit");

    numFetchSuspends
        .name(name + ".numFetchSuspends")
        .desc("Number of times Execute suspended instruction fetching");

    quiesceCycles
        .name(name + ".quiesceCycles")
        .desc("Total number of cycles that CPU has spent quiesced or waiting "
              "for an interrupt")
        .prereq(quiesceCycles);

    loadBlockedIssueCycles
        .name(name + ".loadBlockedIssueCycles")
        .desc("Total number of cycles that CPU has blocked issue waiting "
              "for a load")
        .prereq(loadBlockedIssueCycles);
    loadBlockedIssueInsts
        .name(name + ".loadBlockedIssueInsts")
        .desc("Total number of insts that CPU has blocked issue waiting "
              "for a load")
        .prereq(loadBlockedIssueInsts);
    loadBlockedIssueCPI
        .name(name + ".loadBlockedCPI")
        .desc("LoadBlockedCPI: cycles per instruction")
        .precision(6);
    loadBlockedIssueCPI = loadBlockedIssueCycles / loadBlockedIssueInsts;
    loadBlockedIssueCyclesPercentage
        .name(name + ".loadBlockedCyclesPercengate")
        .desc("Percentage of cycles issue blocked by a load")
        .precision(6);
    loadBlockedIssueCyclesPercentage =
        loadBlockedIssueCycles / baseCpu.numCycles;

    ideaCycles
        .name(name + ".ideaCycles")
        .desc("Ideal inorder cpu cycles")
        .prereq(ideaCycles);
    ideaCyclesNoFUTiming
        .name(name + ".ideaCyclesNoFUTiming")
        .desc("Ideal inorder cpu cycles without FUTiming")
        .prereq(ideaCyclesNoFUTiming);
    ideaCyclesNoLDTiming
        .name(name + ".ideaCyclesNoLDTiming")
        .desc("Ideal inorder cpu cycles without LDTiming")
        .prereq(ideaCyclesNoLDTiming);

    cpi
        .name(name + ".cpi")
        .desc("CPI: cycles per instruction")
        .precision(6);
    cpi = baseCpu.numCycles / numInsts;

    ipc
        .name(name + ".ipc")
        .desc("IPC: instructions per cycle")
        .precision(6);
    ipc = numInsts / baseCpu.numCycles;

    committedInstType
        .init(baseCpu.numThreads, Enums::Num_OpClass)
        .name(name + ".op_class")
        .desc("Class of committed instruction")
        .flags(Stats::total | Stats::pdf | Stats::dist);
    committedInstType.ysubnames(Enums::OpClassStrings);
}

};
