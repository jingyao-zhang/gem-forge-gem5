#include "mcpat_manager.hh"

// Headers for other SimObject's params.
#include "params/Cache.hh"
#include "params/DRAMCtrl.hh"

#include "debug/McPATManager.hh"

#include "cpu/pred/2bit_local.hh"
#include "cpu/pred/tournament.hh"

#if THE_ISA == X86_ISA
#include "arch/x86/tlb.hh"
#define TheISATLBParams X86TLBParams
#elif THE_ISA == RISCV_ISA
#include "arch/riscv/tlb.hh"
#define TheISATLBParams RiscvTLBParams
#else
#error "Unsupported ISA."
#endif

#include "base/output.hh"
#include "cpu/gem_forge/llvm_trace_cpu.hh"
#include "cpu/o3/deriv.hh"
#include "mem/cache/cache.hh"
#include "mem/cache/tags/base.hh"

#include <regex>

McPATManager::McPATManager(const McPATManagerParams *params)
    : SimObject(params), L2Cache(nullptr), statsMap(nullptr) {
  this->xml = new ParseXML();
  this->xml->initialize();

  this->xml->sys.core_tech_node = 90;
  this->xml->sys.device_type = 0;
  this->xml->sys.number_of_L2s = 0;
  this->xml->sys.number_of_L3s = 0;
  this->xml->sys.number_of_NoCs = 0;
  this->xml->sys.number_of_L2Directories = 0;
}

McPATManager::~McPATManager() { delete this->xml; }

void McPATManager::init() {
  inform("Initializing McPATManager.\n");

  for (const auto so : SimObject::getSimObjectList()) {
    DPRINTF(McPATManager, "Init() handling SimObject %s.\n",
            so->name().c_str());

    int cpuId = 0;

    if (so->name() == "system.l2") {
      auto cache = dynamic_cast<const Cache *>(so);
      this->L2Cache = cache;
    }

    else if (so->name() == "system.mem_ctrls") {
      auto params = dynamic_cast<const DRAMCtrlParams *>(so->params());
      this->configureMemoryControl(params);
    }

    else if (McPATManager::isCPU(so, "", cpuId)) {
      if (this->idToCPUMap.count(cpuId) != 0) {
        panic("CPU %d is already added to the map.", cpuId);
      }
      this->idToCPUMap.emplace(cpuId, dynamic_cast<const BaseCPU *>(so));
    }

    else if (McPATManager::isCPU(so, ".dcache", cpuId)) {
      if (this->idToCPUL1DMap.count(cpuId) != 0) {
        panic("CPU L1D %d is already added to the map.", cpuId);
      }
      this->idToCPUL1DMap.emplace(cpuId, dynamic_cast<const BaseCache *>(so));
    }

    else if (McPATManager::isCPU(so, ".l1_5dcache", cpuId)) {
      if (this->idToCPUL1_5DMap.count(cpuId) != 0) {
        panic("CPU L1_5 cache %d is already added to the map.", cpuId);
      }
      this->idToCPUL1_5DMap.emplace(cpuId, dynamic_cast<const BaseCache *>(so));
    }

    else if (McPATManager::isCPU(so, ".dtb", cpuId)) {
      if (this->idToCPUL1DTBMap.count(cpuId) != 0) {
        panic("CPU L1DTB %d is already added to the map.", cpuId);
      }
      this->idToCPUL1DTBMap.emplace(cpuId,
                                    dynamic_cast<const TheISA::TLB *>(so));
    }

    else if (McPATManager::isCPU(so, ".icache", cpuId)) {
      if (this->idToCPUL1IMap.count(cpuId) != 0) {
        panic("CPU L1I %d is already added to the map.", cpuId);
      }
      this->idToCPUL1IMap.emplace(cpuId, dynamic_cast<const Cache *>(so));
    }

    else if (McPATManager::isCPU(so, ".itb", cpuId)) {
      if (this->idToCPUL1ITBMap.count(cpuId) != 0) {
        panic("CPU L1ITB %d is already added to the map.", cpuId);
      }
      this->idToCPUL1ITBMap.emplace(cpuId,
                                    dynamic_cast<const TheISA::TLB *>(so));
    }
  }

  // Configure the LLC.
  int numCPU = this->idToCPUMap.size();
  auto &mcpatSys = this->xml->sys;
  if (this->idToCPUL1_5DMap.empty()) {
    // There is no L1.5
    if (this->L2Cache != nullptr) {
      this->configureL2Cache(this->L2Cache);
      this->configureL2Directories(this->L2Cache);
      mcpatSys.number_of_L2s = 1;
    } else {
      mcpatSys.number_of_L2s = 0;
    }
    mcpatSys.Private_L2 = false;
    mcpatSys.number_of_L3s = 0;
  } else {
    // There is L1.5, use L2 as L3.
    if (this->L2Cache != nullptr) {
      this->configureL3Cache(this->L2Cache);
    }
    mcpatSys.Private_L2 = true;
  }

  mcpatSys.number_of_cores = numCPU;
  mcpatSys.number_of_L1Directories = 0;
  mcpatSys.number_of_L2Directories = 0;
  mcpatSys.number_of_NoCs = 0;
  mcpatSys.homogeneous_cores = 1;
  mcpatSys.core_tech_node = 65;
  mcpatSys.target_core_clockrate = 1000;
  mcpatSys.temperature = 380;
  mcpatSys.number_cache_levels = (this->L2Cache != nullptr) ? 2 : 1;
  mcpatSys.interconnect_projection_type = 0;
  mcpatSys.device_type = 0;
  mcpatSys.longer_channel_device = 1;
  mcpatSys.power_gating = 0;

  if (static_cast<Arch>(THE_ISA) != Arch::X86ISA) {
    panic("McPAT do not support other isa than x86 so far.\n");
  }
  /**
   * X86 ONLY.
   */
  mcpatSys.machine_bits = 32;
  mcpatSys.virtual_address_width = 32;
  mcpatSys.physical_address_width = 32;
  mcpatSys.virtual_memory_page_size = 4096;

  for (int i = 0; i < numCPU; ++i) {
    if (this->idToCPUMap.count(i) == 0) {
      panic("Failed to find cpu %d.", i);
    }
    auto cpu = this->idToCPUMap.at(i);
    if (auto o3 = dynamic_cast<const DerivO3CPU *>(cpu)) {
      this->configureDerivO3CPU(o3);
    }

    else if (auto llvmTraceCPU = dynamic_cast<const LLVMTraceCPU *>(cpu)) {
      this->configureLLVMTraceCPU(llvmTraceCPU);
    }

    else {
      panic("Unsupported cpu type.");
    }
  }
}

void McPATManager::startup() {
  this->statsMap = &Stats::nameMap();
  panic_if(this->statsMap == nullptr, "Failed to get the stats named map.");
}

void McPATManager::configureMemoryControl(const DRAMCtrlParams *params) {

  panic_if(params == nullptr, "nullptr for configureMemoryControl.");
  DPRINTF(McPATManager, "Configure McPAT memory control.\n");

  auto &mc = this->xml->sys.mc;

  mc.mc_clock = 1e6 / params->clk_domain->clockPeriod();
  // DRAMCtrl is a single-channel.
  mc.memory_channels_per_mc = 1;
  mc.number_ranks = params->ranks_per_channel;

  /**
   * ASK TONY: Is this correct?
   */
  mc.llc_line_length = params->write_buffer_size;
}

void McPATManager::configureL2Directories(const BaseCache *cache) {
  auto params = dynamic_cast<const BaseCacheParams *>(cache->params());
  panic_if(params == nullptr, "nullptr for configureL2Directory.");
  DPRINTF(McPATManager, "Configure McPAT l2 directory.\n");

  auto L2DIdx = this->xml->sys.number_of_L2Directories;
  const auto maxNumL2D = sizeof(this->xml->sys.L2Directory) /
                         sizeof(this->xml->sys.L2Directory[0]);
  if (L2DIdx == maxNumL2D) {
    panic("Too many L2 directories, maximum %d.", maxNumL2D);
  }
  this->xml->sys.number_of_L2Directories++;

  auto &L2D = this->xml->sys.L2Directory[L2DIdx];

  L2D.clockrate = 1e6 / params->clk_domain->clockPeriod();
}

void McPATManager::configureL2Cache(const BaseCache *cache) {
  auto params = dynamic_cast<const BaseCacheParams *>(cache->params());
  panic_if(params == nullptr, "nullptr for configureL2Cache.");
  DPRINTF(McPATManager, "Configure McPAT l2 cache.\n");

  auto L2Idx = this->xml->sys.number_of_L2s;
  const auto maxNumL2Caches =
      sizeof(this->xml->sys.L2) / sizeof(this->xml->sys.L2[0]);
  if (L2Idx == maxNumL2Caches) {
    panic("Too many L2 caches, maximum %d.", maxNumL2Caches);
  }
  this->xml->sys.number_of_L2s++;

  auto &L2 = this->xml->sys.L2[L2Idx];

  auto tagsParams =
      dynamic_cast<const BaseTagsParams *>(params->tags->params());
  panic_if(tagsParams == nullptr, "Failed to get BaseTagsParams.");

  L2.L2_config[0] = params->size;           // size.
  L2.L2_config[1] = tagsParams->block_size; // block_size.
  L2.L2_config[2] = params->assoc;          // associativity.
  L2.L2_config[3] = 1;                      // bank.

  /**
   * Throughput w.r.t. core clock.
   * The original gem5-mcpat-parser use hit_lat + resp_lat.
   *
   * However, gem5 has changed its implementation such that
   * there is no hit_lat but access_lat, which is computed as:
   *
   * if (sequential_access) {
   *   access_lat = tag_latency + data_latency;
   * } else {
   *   access_lat = max(tag_latency, data_latency);
   * }
   *
   * Also the original gem5-mcpat-parser sets throughput equal to latency,
   * I assume this means that the cache is not pipelined?
   *
   * Is there a way for me to directly get this protected accessLatency?
   *
   * How to compute this?
   */
  auto accessLatency = McPATManager::getCacheTagAccessLatency(params);
  L2.L2_config[4] = accessLatency; // throughput w.r.t. core clock.
  /**
   * Latency w.r.t. core clock.
   * How to compute this?
   */
  L2.L2_config[5] = accessLatency;
  L2.L2_config[6] = 32; // output_width.
  L2.L2_config[7] = 1;  // cache policy.

  L2.buffer_sizes[0] = params->mshrs;         // miss_buffer_size (MSHR).
  L2.buffer_sizes[1] = params->mshrs;         // fill_buffer_size.
  L2.buffer_sizes[2] = params->mshrs;         // prefetch_buffer_size.
  L2.buffer_sizes[3] = params->write_buffers; // write_back_buffer_size.
  for (int i = 0; i < 8; ++i) {
    DPRINTF(McPATManager, "L2_config[%d] = %d\n", i, L2.L2_config[i]);
  }

  L2.clockrate =
      1e6 / params->clk_domain->clockPeriod(); // clock_rate (what is the unit)?

  /*************************************************************
   * The following fields are not set in the gem5-mcpat-parser.
   * I list them here for completeness sake.
   *************************************************************/
#if 0
  L2.vdd = 0; // vdd (0 means using ITRS default vdd).

  /**
   * 0: no write or write-through with non-write allocate.
   * 1: write-back with write-allocate.
   */
  L2.cache_policy = 1;

  /**
   * -1 means using default power gating virtual power supply voltage
   * constrained by technology and computed automatically.
   */
  L2.power_gating_vcc = -1.0;

  L2.ports[0] = 1; // read ports.
  L2.ports[1] = 1; // write ports.
  L2.ports[2] = 1; // read-write ports.

  /**
   * enum ram_cell_tech_type_num {
   *    itrs_hp = 0,
   *    itrs_lstp = 1,
   *    itrs_lop = 2,
   *    lp_dram = 3,
   *    comm_dram = 4,
   * };
   */
  L2.device_type = 0;
#endif
}

void McPATManager::configureL3Cache(const BaseCache *cache) {
  auto params = dynamic_cast<const BaseCacheParams *>(cache->params());
  panic_if(params == nullptr, "nullptr for configureL2Cache.");
  DPRINTF(McPATManager, "Configure McPAT l2 cache.\n");

  auto L3Idx = this->xml->sys.number_of_L3s;
  const auto maxNumL3Caches =
      sizeof(this->xml->sys.L3) / sizeof(this->xml->sys.L3[0]);
  if (L3Idx == maxNumL3Caches) {
    panic("Too many L3 caches, maximum %d.", maxNumL3Caches);
  }
  this->xml->sys.number_of_L3s++;

  auto &L3 = this->xml->sys.L3[L3Idx];

  auto tagsParams =
      dynamic_cast<const BaseTagsParams *>(params->tags->params());
  panic_if(tagsParams == nullptr, "Failed to get BaseTagsParams.");

  L3.L3_config[0] = params->size;           // size.
  L3.L3_config[1] = tagsParams->block_size; // block_size.
  L3.L3_config[2] = params->assoc;          // associativity.
  L3.L3_config[3] = 1;                      // bank.

  auto accessLatency = McPATManager::getCacheTagAccessLatency(params);
  L3.L3_config[4] = accessLatency; // throughput w.r.t. core clock.
  /**
   * Latency w.r.t. core clock.
   * How to compute this?
   */
  L3.L3_config[5] = accessLatency;
  L3.L3_config[6] = 32; // output_width.
  L3.L3_config[7] = 1;  // cache policy.

  L3.buffer_sizes[0] = params->mshrs;         // miss_buffer_size (MSHR).
  L3.buffer_sizes[1] = params->mshrs;         // fill_buffer_size.
  L3.buffer_sizes[2] = params->mshrs;         // prefetch_buffer_size.
  L3.buffer_sizes[3] = params->write_buffers; // write_back_buffer_size.
  for (int i = 0; i < 8; ++i) {
    DPRINTF(McPATManager, "L3_config[%d] = %d\n", i, L3.L3_config[i]);
  }

  L3.clockrate =
      1e6 / params->clk_domain->clockPeriod(); // clock_rate (what is the unit)?
}
void McPATManager::configureDerivO3CPU(const DerivO3CPU *cpu) {
  auto idx = cpu->cpuId();
  auto params = McPATManager::getParams<DerivO3CPUParams>(cpu);
  panic_if(params == nullptr, "failed to get the params for o3 cpu.");
  panic_if(idx >= this->xml->sys.number_of_cores, "cpu idx overflows.");

  DPRINTF(McPATManager, "configure mcpat o3 cpu %d.\n", idx);

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
  int maxBaseStages =
      std::max({params->commitToDecodeDelay, params->commitToFetchDelay,
                params->commitToIEWDelay, params->commitToRenameDelay});

  core.pipeline_depth[0] = intExe + baseStages + maxBaseStages;
  core.pipeline_depth[1] = fpExe + baseStages + maxBaseStages;

  core.instruction_buffer_size = params->fetchBufferSize;

  /**
   * Again gem5 does not distinguish int/fp instruction window.
   */
  core.instruction_window_size = params->numIQEntries;
  core.fp_instruction_window_size = params->numIQEntries;

  core.ROB_size = params->numROBEntries;
  core.phy_Regs_IRF_size = params->numPhysIntRegs;
  core.phy_Regs_FRF_size = params->numPhysFloatRegs;
  core.store_buffer_size = params->SQEntries;
  core.load_buffer_size = params->LQEntries;

  /**
   * X86 ONLY
   */
  core.opt_local = 0;
  core.instruction_length = 32;
  core.opcode_width = 16;
  core.micro_opcode_width = 8;
  core.machine_type = 0; // 0 for O3.

  if (auto branchPredictor = params->branchPred) {
    /**
     * Branch predictor.
     */
    auto branchPredParams =
        McPATManager::getParams<BranchPredictorParams>(branchPredictor);
    core.RAS_size = branchPredParams->RASSize;

    // BTB config.
    auto &mcpatBTB = core.BTB;
    mcpatBTB.BTB_config[0] = 6;
    mcpatBTB.BTB_config[1] = branchPredParams->BTBEntries;
    mcpatBTB.BTB_config[2] = 6;
    mcpatBTB.BTB_config[3] = 2;
    mcpatBTB.BTB_config[4] = 1;
    mcpatBTB.BTB_config[5] = 1;

    auto &mcpatPredictor = core.predictor;
    this->configureBranchPredictor(branchPredictor, mcpatPredictor);
  }

  if (auto instTLB = dynamic_cast<TheISA::TLB *>(params->itb)) {
    /**
     * Instruction TLB.
     */
    auto &mcpatITLB = core.itlb;
    mcpatITLB.number_entries = McPATManager::getTLBSize(instTLB);
  }

  if (auto dataTLB = dynamic_cast<TheISA::TLB *>(params->dtb)) {
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
    auto accessLatency = McPATManager::getCacheTagAccessLatency(instL1Params);
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

  if (this->idToCPUL1_5DMap.count(idx) != 0) {
    /**
     * Private L2 cache.
     */
    auto L1_5Cache = this->idToCPUL1_5DMap.at(idx);
    this->configureL2Cache(L1_5Cache);
    this->configureL2Directories(L1_5Cache);
  }

  if (this->idToCPUL1DMap.count(idx) != 0) {
    /**
     * L1 data cache.
     */
    auto dataL1 = this->idToCPUL1DMap.at(idx);
    auto dataL1Params = McPATManager::getParams<CacheParams>(dataL1);
    auto dataL1TagParams =
        McPATManager::getParams<BaseTagsParams>(dataL1Params->tags);
    auto accessLatency = McPATManager::getCacheTagAccessLatency(dataL1Params);
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

void McPATManager::configureBranchPredictor(
    const BPredUnit *predictor, predictor_systemcore &mcpatPredictor) {
  if (auto localPredictor = dynamic_cast<const LocalBP *>(predictor)) {
    auto params = McPATManager::getParams<LocalBPParams>(localPredictor);
    mcpatPredictor.local_predictor_entries = params->localPredictorSize;
    mcpatPredictor.local_predictor_size[0] = static_cast<int>(
        std::floor(std::log2(static_cast<double>(params->localPredictorSize))));
    mcpatPredictor.local_predictor_size[1] = params->localCtrBits;
    return;
  }

  if (auto tournamentPredictor =
          dynamic_cast<const TournamentBP *>(predictor)) {
    auto params =
        McPATManager::getParams<TournamentBPParams>(tournamentPredictor);

    mcpatPredictor.local_predictor_entries = params->localPredictorSize;
    mcpatPredictor.local_predictor_size[0] = static_cast<int>(
        std::floor(std::log2(static_cast<double>(params->localPredictorSize))));
    mcpatPredictor.local_predictor_size[1] = params->localCtrBits;

    mcpatPredictor.global_predictor_entries = params->globalPredictorSize;
    mcpatPredictor.global_predictor_bits = params->globalCtrBits;

    mcpatPredictor.chooser_predictor_entries = params->choicePredictorSize;
    mcpatPredictor.chooser_predictor_bits = params->choiceCtrBits;
    return;
  }

  panic("Unsupported type of predictor %s.", predictor->name().c_str());
}

bool McPATManager::isCPU(const SimObject *so, const std::string &suffix,
                         int &cpuId) {
  const auto &name = so->name();
#if defined(__GNUC__) && __GNUC__ < 5
  /**
   * GCC 4.9 implements regex. Manually do the checking for GCC < 5.0.
   */
  const std::string prefix = "system.cpu";
  auto prefixPos = name.find("system.cpu");
  if (prefixPos == std::string::npos) {
    return false;
  }

  auto idxStartPos = prefixPos + prefix.size();
  auto idxStopPos = idxStartPos;
  while (idxStopPos < name.size() && std::isdigit(name[idxStopPos])) {
    idxStopPos++;
  }

  if (name.substr(idxStopPos) != suffix) {
    return false;
  }

  auto idSize = idxStopPos - idxStartPos;
  if (idSize > 0) {
    // There is a number.
    cpuId = std::stoi(name.substr(idxStartPos, idSize));
  } else {
    cpuId = 0;
  }
  inform("%s belongs to cpu %s.\n", name.c_str(), cpuId);
  return true;

#else
  auto expression = "system.cpu([0-9]*)" + suffix;
  try {
    std::regex regex(expression);
    std::smatch matches;
    if (std::regex_match(name, matches, regex)) {
      // The first sub_match is the whole string.
      // The next sub_match is the first parenthesized expression.
      if (matches.size() == 2) {
        auto cpuIdMatch = matches[1].str();
        if (!cpuIdMatch.empty()) {
          cpuId = std::stoi(cpuIdMatch);
        } else {
          // There is only one cpu.
          cpuId = 0;
        }
        inform("%s belongs to cpu %s.\n", name.c_str(), cpuId);
      }
      return true;
    }
    return false;
  } catch (const std::regex_error &e) {
    fatal("regex caught: %s.", e.what());
  }
#endif
}

int McPATManager::getTLBSize(const TheISA::TLB *tlb) {
  switch (static_cast<Arch>(THE_ISA)) {
  case Arch::X86ISA: {
    return McPATManager::getParams<TheISATLBParams>(tlb)->size;
  }
  default: { panic("Unsupported ISA %s for get TLBSize.", THE_ISA_STR); }
  }
}

int McPATManager::getCacheTagAccessLatency(const BaseCacheParams *cacheParams) {
  auto accessLatency =
      cacheParams->sequential_access
          ? (cacheParams->data_latency + cacheParams->tag_latency)
          : std::max(cacheParams->data_latency, cacheParams->tag_latency);
  return accessLatency;
}

void McPATManager::computeEnergy() {

  panic_if(this->statsMap == nullptr, "StatsMap is still nullptr.");

  {
    auto &stats = Stats::statsList();
    // First we have to prepare all of them.
    for (auto stat : stats) {
      stat->enable();
    }
    for (auto stat : stats) {
      stat->prepare();
    }
  }

  this->setStatsSystem();
  this->setStatsMemoryControl();
  this->setStatsL2Cache();
  for (int idx = 0; idx < this->idToCPUMap.size(); ++idx) {
    if (dynamic_cast<const DerivO3CPU *>(this->idToCPUMap.at(idx))) {
      this->setStatsDerivO3CPU(idx);
    }

    else if (dynamic_cast<const LLVMTraceCPU *>(this->idToCPUMap.at(idx))) {
      this->setStatsLLVMTraceCPU(idx);
    }

    else {
      panic("Unsupported type of cpu to set stats.");
    }
  }

  Processor processor(this->xml);

  auto &stream = *simout.findOrCreate("mcpat.txt")->stream();
  processor.dumpToFile(stream);
  // processor.displayEnergy(2);
}

void McPATManager::setStatsSystem() {
  auto &sys = this->xml->sys;
  auto ticks = this->getScalarStats("sim_ticks");
  if (!this->idToCPUMap.empty()) {
    auto cycles = ticks / sys.core[0].clock_rate;
    sys.total_cycles = cycles;
  } else {
    // What should I do here?
    panic("Must have at least one cpu.");
  }
}

void McPATManager::setStatsMemoryControl() {
  auto memReads = this->getVecStatsTotal("system.mem_ctrls.num_reads");
  auto memWrites = this->getVecStatsTotal("system.mem_ctrls.num_writes");

  auto &mc = this->xml->sys.mc;
  mc.memory_reads = memReads;
  mc.memory_writes = memWrites;
  mc.memory_accesses = memReads + memWrites;

  DPRINTF(McPATManager, "Set stats memory control reads %f writes %f.\n",
          memReads, memWrites);
}

void McPATManager::setStatsL2Cache() {
  if (this->L2Cache == nullptr) {
    return;
  }

  auto total = this->getVecStatsTotal("system.l2.overall_accesses");
  auto totalMisses = this->getVecStatsTotal("system.l2.overall_misses");
  auto writes = this->getVecStatsTotal("system.l2.WritebackClean_accesses") +
                this->getVecStatsTotal("system.l2.WritebackDirty_accesses");
  auto writeMisses = this->getVecStatsTotal("system.l2.WritebackClean_misses") +
                     this->getVecStatsTotal("system.l2.WritebackDirty_misses");

  if (this->idToCPUL1_5DMap.empty()) {
    /**
     * No L1.5 cache.
     */
    auto &L2 = this->xml->sys.L2[0];
    L2.read_accesses = total - writes;
    L2.write_accesses = writes;
    L2.read_misses = totalMisses - writeMisses;
    L2.write_misses = writeMisses;
  } else {
    /**
     * Use L2 cache as L3 cace.
     */
    auto &L3 = this->xml->sys.L3[0];
    L3.read_accesses = total - writes;
    L3.write_accesses = writes;
    L3.read_misses = totalMisses - writeMisses;
    L3.write_misses = writeMisses;
  }
}

void McPATManager::setStatsDerivO3CPU(int idx) {
  auto &core = this->xml->sys.core[idx];
  auto cpu = this->idToCPUMap.at(idx);
  auto params = McPATManager::getParams<DerivO3CPUParams>(cpu);

#define scalar(x) this->getScalarStats(x, idx)
#define vector(x) this->getVecStatsTotal(x, idx)
#define formula(x) this->getFormulaStats(x, idx)

  auto decodedInsts = scalar("decode.DecodedInsts");
  auto branchInsts = scalar("fetch.Branches");
  auto loadInsts = vector("iew.iewExecLoadInsts");
  auto storeInsts = formula("iew.exec_stores");
  auto commitInsts = vector("commit.committedInsts");
  auto commitIntInsts = vector("commit.int_insts");
  auto commitFpInsts = vector("commit.fp_insts");
  auto totalCycles = scalar("numCycles");
  auto idleCycles = scalar("idleCycles");
  auto robReads = scalar("rob.rob_reads");
  auto robWrites = scalar("rob.rob_writes");

  /**
   * Gem5 seems not distinguish rename int/fp operands.
   * Just make rename float writes 0.
   */
  auto renameWrites = scalar("rename.RenamedOperands");
  auto renameReads = scalar("rename.RenameLookups");
  auto renameFpReads = scalar("rename.fp_rename_lookups");
  auto renameFpWrites = 0;

  auto instWinReads = scalar("iq.int_inst_queue_reads");
  auto instWinWrites = scalar("iq.int_inst_queue_writes");
  auto instWinWakeUpAccesses = scalar("iq.int_inst_queue_wakeup_accesses");
  auto instWinFpReads = scalar("iq.fp_inst_queue_reads");
  auto instWinFpWrites = scalar("iq.fp_inst_queue_writes");
  auto instWinFpWakeUpAccesses = scalar("iq.fp_inst_queue_wakeup_accesses");

  auto intRegReads = scalar("int_regfile_reads");
  auto intRegWrites = scalar("int_regfile_writes");
  auto fpRegReads = scalar("fp_regfile_reads");
  auto fpRegWrites = scalar("fp_regfile_writes");

  auto commitCalls = vector("commit.function_calls");

  auto intALU = scalar("iq.int_alu_accesses");
  auto fpALU = scalar("iq.fp_alu_accesses");
  // auto multi = this->getHistStats()
  auto multi = 0.0;
  auto divs = 0.0;
  auto multiAndDiv = multi + divs;
  intALU -= multiAndDiv;

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

  core.inst_window_reads = instWinReads;
  core.inst_window_writes = instWinWrites;
  core.inst_window_wakeup_accesses = instWinWakeUpAccesses;
  core.inst_window_selections = 0;
  core.fp_inst_window_reads = instWinFpReads;
  core.fp_inst_window_writes = instWinFpWrites;
  core.fp_inst_window_wakeup_accesses = instWinFpWakeUpAccesses;
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
  core.ialu_accesses = intALU;
  core.fpu_accesses = fpALU;
  core.mul_accesses = multiAndDiv;
  core.cdb_alu_accesses = intALU;
  core.cdb_mul_accesses = multiAndDiv;
  core.cdb_fpu_accesses = fpALU;
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

  if (params->branchPred) {
    /**
     * Branch predictor.
     */
    auto &mcpatBTB = core.BTB;

    auto reads = scalar("branchPred.BTBLookups");

    mcpatBTB.read_accesses = reads;

    /**
     * ASK TONY: Gem5 seems missing the stats.
     */
    mcpatBTB.write_accesses = 0;

    auto predictorAccesses = this->getScalarStats("branchPred.lookups", idx);
    core.predictor.predictor_accesses = predictorAccesses;
  }

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

  if (this->idToCPUL1_5DMap.count(idx) != 0) {
    auto writes = vector("l1_5dcache.WritebackDirty_accesses") +
                  vector("l1_5dcache.WritebackClean_accesses");
    auto writeHits = vector("l1_5dcache.WritebackDirty_hits") +
                     vector("l1_5dcache.WritebackClean_hits");
    auto writeMisses = writes - writeHits;
    auto reads = vector("l1_5dcache.ReadReq_accesses") +
                 vector("l1_5dcache.ReadExReq_accesses") +
                 vector("l1_5dcache.ReadSharedReq_accesses");
    auto readMisses = vector("l1_5dcache.ReadReq_misses") +
                      vector("l1_5dcache.ReadExReq_misses") +
                      vector("l1_5dcache.ReadSharedReq_misses");
    auto &L2 = this->xml->sys.L2[idx];
    L2.read_accesses = reads;
    L2.write_accesses = writes;
    L2.read_misses = readMisses;
    L2.write_misses = writeMisses;
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

// void McPATManager::setStatsL2Directories(const Cache *cache);

Stats::Info *McPATManager::getStat(const std::string &stat) {
  auto statsIter = this->statsMap->find(stat);
  panic_if(statsIter == this->statsMap->end(), "Failed to find the stat %s.",
           stat.c_str());
  return statsIter->second;
}

std::string McPATManager::getStatName(const std::string &stat, int cpu) {
  auto statName = stat;
  if (cpu != -1) {
    if (this->idToCPUMap.size() > 1) {
      statName = std::string("system.cpu") + std::to_string(cpu) + "." + stat;
    } else {
      statName = std::string("system.cpu.") + stat;
    }
  }
  return statName;
}

double McPATManager::getScalarStats(const std::string &stat, int cpu) {
  auto statName = this->getStatName(stat, cpu);
  auto scalar = dynamic_cast<Stats::ScalarInfo *>(this->getStat(statName));
  panic_if(scalar == nullptr, "Stat %s is not scalar.", statName.c_str());
  return scalar->result();
}

double McPATManager::getVecStatsTotal(const std::string &stat, int cpu) {
  auto statName = this->getStatName(stat, cpu);
  auto vector = dynamic_cast<Stats::VectorInfo *>(this->getStat(statName));
  panic_if(vector == nullptr, "Stat %s is not vector.", statName.c_str());
  return vector->total();
}

double McPATManager::getFormulaStats(const std::string &stat, int cpu) {
  auto statName = this->getStatName(stat, cpu);
  auto scalar = dynamic_cast<Stats::FormulaInfo *>(this->getStat(statName));
  panic_if(scalar == nullptr, "Stat %s is not formula.", statName.c_str());
  DPRINTF(McPATManager, "Get formular %s with value %f.\n", statName.c_str(),
          scalar->total());
  return scalar->total();
}

void McPATManager::dump() {
  inform("McPATManager dump called in cpp world.\n");

  // Check if the user initialized the manager.
  for (auto so : SimObject::getSimObjectList()) {
    if (auto manager = dynamic_cast<McPATManager *>(so)) {
      // Found myself.
      manager->computeEnergy();
      return;
    }
  }
}

McPATManager *McPATManagerParams::create() { return new McPATManager(this); }