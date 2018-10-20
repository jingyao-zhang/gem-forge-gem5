#include "mcpat_manager.hh"

#include "debug/McPATManager.hh"

#include "cpu/llvm_trace/llvm_trace_cpu.hh"
#include "mem/cache/cache.hh"
#include "mem/cache/tags/base.hh"

/**
 * Separate the support of LLVMTraceCPU out in another file for cleanness.
 *
 * All the missing pieces from LLVMTraceCPu.
 *
 * 1. Back signal delay.
 * 2. Number of physical registers.
 *
 *
 */

void McPATManager::configureLLVMTraceCPU(const LLVMTraceCPU *cpu) {
  auto idx = cpu->cpuId();
  auto params = McPATManager::getParams<LLVMTraceCPUParams>(cpu);
  panic_if(params == nullptr, "failed to get the params for LLVMTraceCPU.");
  panic_if(idx >= this->xml->sys.number_of_cores, "cpu idx overflows.");

  DPRINTF(McPATManager, "configure mcpat LLVMTraceCPU %d.\n", idx);

  auto &core = this->xml->sys.core[idx];
  core.clock_rate = 1e6 / params->clk_domain->clockPeriod();
  core.x86 = static_cast<Arch>(THE_ISA) == Arch::X86ISA;
  core.fetch_width = params->fetchWidth;
  core.decode_width = params->decodeWidth;
  /**
   * Make the peak issueWidth the same as issueWidth;
   */
  core.issue_width = params->issueWidth;
  core.peak_issue_width = params->issueWidth;
  core.commit_width = params->commitWidth;

  /**
   * There is no float issue width in gem5.
   * Make it min(issueWidth, numFPU).
   */
  core.fp_issue_width = std::min(params->issueWidth, 6u);

  /**
   * Integer pipeline and float pipeline depth.
   */
  int intExe = 3;
  int fpExe = 6;
  if (static_cast<Arch>(THE_ISA) == Arch::X86ISA) {
    intExe = 2;
    fpExe = 8;
  } else if (static_cast<Arch>(THE_ISA) == Arch::ArmISA) {
    intExe = 3;
    fpExe = 7;
  }
  int baseStages = params->fetchToDecodeDelay + params->decodeToRenameDelay +
                   params->renameToIEWDelay + params->iewToCommitDelay;
  /**
   * Missing-1: LLVMTraceCPU has no back propagation delay, so we use the
   * default values from DerivO3CPU, which is all 1.
   */
  int maxBaseStages = std::max({1, 1, 1, 1});

  core.pipeline_depth[0] = intExe + baseStages + maxBaseStages;
  core.pipeline_depth[1] = fpExe + baseStages + maxBaseStages;

  core.instruction_buffer_size = params->maxFetchQueueSize;

  /**
   * Again gem5 does not distinguish int/fp instruction window.
   */
  core.instruction_window_size = params->maxInstructionQueueSize;
  core.fp_instruction_window_size = params->maxInstructionQueueSize;

  core.ROB_size = params->maxReorderBufferSize;

  /**
   * Missing-2: LLVMTraceCPU actually has infinite number of registers.
   */
  core.phy_Regs_IRF_size = 256;
  core.phy_Regs_FRF_size = 256;
  core.store_buffer_size = params->storeQueueSize;
  core.load_buffer_size = params->loadQueueSize;

  /**
   * X86 ONLY
   */
  core.opt_local = 0;
  core.instruction_length = 32;
  core.opcode_width = 16;
  core.micro_opcode_width = 8;
  core.machine_type = 0; // 0 for O3.

  // TODO: refactor this into a separate funciton.

  if (auto instTLB = params->itb) {
    /**
     * Instruction TLB.
     */
    auto &mcpatITLB = core.itlb;
    mcpatITLB.number_entries = McPATManager::getTLBSize(instTLB);
  }

  if (auto dataTLB = params->dtb) {
    /**
     * Data TLB.
     */
    auto &mcpatDTLB = core.dtlb;
    mcpatDTLB.number_entries = McPATManager::getTLBSize(dataTLB);
  }

  if (this->idToCPUL1IMap.count(idx) != 0) {
    /**
     * L1 inst cache.
     */
    auto instL1 = this->idToCPUL1IMap.at(idx);
    auto instL1Params = McPATManager::getParams<CacheParams>(instL1);
    auto instL1TagParams =
        McPATManager::getParams<BaseTagsParams>(instL1Params->tags);
    auto accessLatency =
        McPATManager::getCacheTagAccessLatency(instL1TagParams);
    auto &mcpatInstL1 = core.icache;

    mcpatInstL1.icache_config[0] = instL1Params->size;          // capacity
    mcpatInstL1.icache_config[1] = instL1TagParams->block_size; // block_width
    mcpatInstL1.icache_config[2] = instL1Params->assoc;         // associativity
    mcpatInstL1.icache_config[3] = 1;                           // bank
    mcpatInstL1.icache_config[4] = accessLatency;               // throughput
    mcpatInstL1.icache_config[5] = accessLatency;               // latency
    mcpatInstL1.icache_config[6] = 32;                          // output_width
    mcpatInstL1.icache_config[7] = 0;                           // cache_policy
    for (int i = 0; i < 8; ++i) {
      DPRINTF(McPATManager, "icache_config[%d] = %d\n", i,
              mcpatInstL1.icache_config[i]);
    }

    mcpatInstL1.buffer_sizes[0] = instL1Params->mshrs; // MSHR
    mcpatInstL1.buffer_sizes[1] = instL1Params->mshrs; // fill_buffer_size
    mcpatInstL1.buffer_sizes[2] = instL1Params->mshrs; // prefetch_buffer_size
    mcpatInstL1.buffer_sizes[3] = 0;                   // wb_buffer_size
    for (int i = 0; i < 4; ++i) {
      DPRINTF(McPATManager, "icache_buffer_sizes[%d] = %d\n", i,
              mcpatInstL1.buffer_sizes[i]);
    }
  }

  if (this->idToCPUL1DMap.count(idx) != 0) {
    /**
     * L1 data cache.
     */
    auto dataL1 = this->idToCPUL1DMap.at(idx);
    auto dataL1Params = McPATManager::getParams<CacheParams>(dataL1);
    auto dataL1TagParams =
        McPATManager::getParams<BaseTagsParams>(dataL1Params->tags);
    auto accessLatency =
        McPATManager::getCacheTagAccessLatency(dataL1TagParams);
    auto &mcpatDataL1 = core.dcache;

    mcpatDataL1.dcache_config[0] = dataL1Params->size;          // capacity
    mcpatDataL1.dcache_config[1] = dataL1TagParams->block_size; // block_width
    mcpatDataL1.dcache_config[2] = dataL1Params->assoc;         // associativity
    mcpatDataL1.dcache_config[3] = 1;                           // bank
    mcpatDataL1.dcache_config[4] = accessLatency;               // throughput
    mcpatDataL1.dcache_config[5] = accessLatency;               // latency
    mcpatDataL1.dcache_config[6] = 32;                          // output_width
    mcpatDataL1.dcache_config[7] = 0;                           // cache_policy
    for (int i = 0; i < 8; ++i) {
      DPRINTF(McPATManager, "dcache_config[%d] = %d\n", i,
              mcpatDataL1.dcache_config[i]);
    }

    mcpatDataL1.buffer_sizes[0] = dataL1Params->mshrs; // MSHR
    mcpatDataL1.buffer_sizes[1] = dataL1Params->mshrs; // fill_buffer_size
    mcpatDataL1.buffer_sizes[2] = dataL1Params->mshrs; // prefetch_buffer_size
    mcpatDataL1.buffer_sizes[3] = 0;                   // wb_buffer_size
  }

  /**
   * L1 directory.
   */
  auto &L1Directory = this->xml->sys.L1Directory[idx];
  L1Directory.clockrate = 1e6 / params->clk_domain->clockPeriod();
}

void McPATManager::setStatsLLVMTraceCPU(int idx) {

  auto &core = this->xml->sys.core[idx];
  auto cpu = this->idToCPUMap.at(idx);
  auto params = McPATManager::getParams<LLVMTraceCPUParams>(cpu);

#define scalar(x) this->getScalarStats(x, idx)
#define vector(x) this->getVecStatsTotal(x, idx)
#define formula(x) this->getFormulaStats(x, idx)

  auto totalCycles = scalar("numCycles");
  auto idleCycles = 0;

  auto branchInsts = scalar("fetch.Branches");
  auto decodedInsts = scalar("decode.DecodedInsts");

  /**
   * Gem5 seems not distinguish rename int/fp operands.
   * Just make rename float writes 0.
   */
  auto renameWrites = scalar("rename.RenamedOperands");
  auto renameReads = scalar("rename.RenameLookups");
  auto renameFpReads = scalar("rename.fp_rename_lookups");
  auto renameFpWrites = 0;

  auto robReads = scalar("iew.robReads");
  auto robWrites = scalar("iew.robWrites");

  auto instWinIntReads = scalar("iew.intInstQueueReads");
  auto instWinIntWrites = scalar("iew.intInstQueueWrites");
  auto instWinIntWakeups = scalar("iew.intInstQueueWakeups");
  auto instWinFpReads = scalar("iew.fpInstQueueReads");
  auto instWinFpWrites = scalar("iew.fpInstQueueWrites");
  auto instWinFpWakeups = scalar("iew.fpInstQueueWakeups");

  auto intRegReads = scalar("iew.intRegReads");
  auto intRegWrites = scalar("iew.intRegWrites");
  auto fpRegReads = scalar("iew.fpRegReads");
  auto fpRegWrites = scalar("iew.fpRegWrites");

  auto ALUAccesses = scalar("iew.ALUAccessesCycles");
  auto MultAccesses = scalar("iew.MultAccessesCycles");
  auto FPUAccesses = scalar("iew.FPUAccessesCycles");

  auto loadInsts = scalar("iew.execLoadInsts");
  auto storeInsts = scalar("iew.execStoreInsts");

  auto commitInsts = vector("commit.committedInsts");
  auto commitIntInsts = vector("commit.committedIntInsts");
  auto commitFpInsts = vector("commit.committedFpInsts");
  auto commitCalls = vector("commit.committedCallInsts");

  core.total_instructions = decodedInsts;
  core.int_instructions = 0;
  core.fp_instructions = 0;
  core.branch_instructions = branchInsts;
  core.branch_mispredictions = 0;
  core.committed_instructions = commitInsts;
  core.committed_int_instructions = commitIntInsts;
  core.committed_fp_instructions = commitFpInsts;
  core.load_instructions = loadInsts;
  core.store_instructions = storeInsts;
  core.total_cycles = totalCycles;
  core.idle_cycles = idleCycles;
  core.busy_cycles = totalCycles - idleCycles;
  core.instruction_buffer_reads = 0;
  core.instruction_buffer_write = 0;
  core.ROB_reads = robReads;
  core.ROB_writes = robWrites;

  core.rename_reads = renameReads;
  core.rename_writes = renameWrites;
  core.fp_rename_reads = renameFpReads;
  core.fp_rename_writes = renameFpWrites;

  core.inst_window_reads = instWinIntReads;
  core.inst_window_writes = instWinIntWrites;
  core.inst_window_wakeup_accesses = instWinIntWakeups;
  core.inst_window_selections = 0;
  core.fp_inst_window_reads = instWinFpReads;
  core.fp_inst_window_writes = instWinFpWrites;
  core.fp_inst_window_wakeup_accesses = instWinFpWakeups;
  core.fp_inst_window_selections = 0;
  core.archi_int_regfile_reads = 0;
  core.archi_float_regfile_reads = 0;
  core.phy_int_regfile_reads = 0;
  core.phy_float_regfile_reads = 0;
  core.phy_int_regfile_writes = 0;
  core.phy_float_regfile_writes = 0;
  core.archi_int_regfile_writes = 0;
  core.archi_float_regfile_writes = 0;
  core.int_regfile_reads = intRegReads;
  core.float_regfile_reads = fpRegReads;
  core.int_regfile_writes = intRegWrites;
  core.float_regfile_writes = fpRegWrites;
  core.windowed_reg_accesses = 0;
  core.windowed_reg_transports = 0;
  core.function_calls = commitCalls;
  core.context_switches = 0;
  core.ialu_accesses = ALUAccesses;
  core.mul_accesses = MultAccesses;
  core.fpu_accesses = FPUAccesses;
  core.cdb_alu_accesses = ALUAccesses;
  core.cdb_mul_accesses = MultAccesses;
  core.cdb_fpu_accesses = FPUAccesses;
  core.load_buffer_reads = 0;
  core.load_buffer_writes = 0;
  core.load_buffer_cams = 0;
  core.store_buffer_reads = 0;
  core.store_buffer_writes = 0;
  core.store_buffer_cams = 0;
  core.store_buffer_forwards = 0;
  core.main_memory_access = 0;
  core.main_memory_read = 0;
  core.main_memory_write = 0;
  core.pipeline_duty_cycle = 0;

  //   if (params->branchPred) {
  //     /**
  //      * Branch predictor.
  //      */
  //     auto &mcpatBTB = core.BTB;

  //     auto reads = scalar("branchPred.BTBLookups");

  //     mcpatBTB.read_accesses = reads;

  //     /**
  //      * ASK TONY: Gem5 seems missing the stats.
  //      */
  //     mcpatBTB.write_accesses = 0;

  //     auto predictorAccesses = this->getScalarStats("branchPred.lookups",
  //     idx); core.predictor.predictor_accesses = predictorAccesses;
  //   }

  if (params->dtb) {
    auto reads = this->getScalarStats("dtb.rdAccesses", idx);
    auto readMisses = this->getScalarStats("dtb.rdMisses", idx);
    auto writes = this->getScalarStats("dtb.wrAccesses", idx);
    auto writeMisses = this->getScalarStats("dtb.wrMisses", idx);
    core.dtlb.total_accesses = reads + writes;
    core.dtlb.read_accesses = reads;
    core.dtlb.write_accesses = writes;
    core.dtlb.read_misses = readMisses;
    core.dtlb.write_misses = writeMisses;
    core.dtlb.total_misses = readMisses + writeMisses;
  }

  if (params->itb) {
    auto reads = this->getScalarStats("itb.rdAccesses", idx);
    auto readMisses = this->getScalarStats("itb.rdMisses", idx);
    auto writes = this->getScalarStats("itb.wrAccesses", idx);
    auto writeMisses = this->getScalarStats("itb.wrMisses", idx);
    core.itlb.total_accesses = reads + writes;
    core.itlb.total_misses = readMisses + writeMisses;
  }

  if (this->idToCPUL1DMap.count(idx) != 0) {
    auto writes = vector("dcache.WriteReq_accesses");
    auto writeMisses = vector("dcache.WriteReq_misses");
    auto reads = vector("dcache.ReadReq_accesses");
    auto readMisses = vector("dcache.ReadReq_misses");

    auto &mcpatDataL1 = core.dcache;
    mcpatDataL1.read_accesses = reads;
    mcpatDataL1.write_accesses = writes;
    mcpatDataL1.read_misses = readMisses;
    mcpatDataL1.write_misses = writeMisses;
  }

  if (this->idToCPUL1IMap.count(idx) != 0) {
    auto reads = vector("icache.ReadReq_accesses");
    auto readMisses = vector("icache.ReadReq_misses");

    auto &mcpatInstL1 = core.icache;
    mcpatInstL1.read_accesses = reads;
    mcpatInstL1.read_misses = readMisses;
  }

#undef scalar
#undef vector
#undef formula
}