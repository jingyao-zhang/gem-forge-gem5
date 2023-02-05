#ifndef __MCPAT_MCAPT_MANAGER_HH__
#define __MCPAT_MCAPT_MANAGER_HH__

// #include "base/misc.hh""
#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "params/McPATManager.hh"
#include "sim/sim_object.hh"

// Headers from McPAT.
#include "mcpat-1.3/XML_Parse.h"
#include "mcpat-1.3/processor.h"

#include <unordered_map>

class BPredUnit;
class BaseTagsParams;
class DRAMInterfaceParams;
class BaseCache;
class BaseCacheParams;
class Cache;
class BaseCPU;
class DerivO3CPU;
class LLVMTraceCPU;

namespace TheISA {
class TLB;
} // namespace TheISA

class McPATManager : public SimObject {
public:
  McPATManager(const McPATManagerParams *params);
  ~McPATManager();

  void init() override;
  void startup() override;
  // void regStats() override { this->mcpatStats.regStats(); }

  void computeEnergy();
  static void dump();

private:
  ParseXML *xml;

  std::unordered_map<int, const BaseCPU *> idToCPUMap;
  std::unordered_map<int, const BaseCache *> idToCPUL1DMap;
  std::unordered_map<int, const TheISA::TLB *> idToCPUL1DTBMap;
  std::unordered_map<int, const BaseCache *> idToCPUL1_5DMap;
  std::unordered_map<int, const Cache *> idToCPUL1IMap;
  std::unordered_map<int, const TheISA::TLB *> idToCPUL1ITBMap;

  const Cache *L2Cache;

  Stats::NameMapType *statsMap;

  /**
   * This breaks the object orientated principle. But I don't want the
   * modification scatters in the source tree.
   */
  void configureMemoryControl(const DRAMInterfaceParams *params);
  void configureL2Cache(const BaseCache *cache);
  void configureL2Directories(const BaseCache *cache);
  void configureL3Cache(const BaseCache *cache);
  void configureDerivO3CPU(const DerivO3CPU *cpu);
  void configureLLVMTraceCPU(const LLVMTraceCPU *cpu);
  void configureBranchPredictor(const BPredUnit *predictor,
                                predictor_systemcore &mcpatPredictor);

  void setStatsSystem();
  void setStatsMemoryControl();
  void setStatsL2Cache();
  // void setStatsL2Directories(const Cache *cache);
  void setStatsDerivO3CPU(int idx);
  void setStatsLLVMTraceCPU(int idx);

  Stats::Info *getStat(const std::string &stat);
  std::string getStatName(const std::string &stat, int cpu);
  /**
   * If cpu != -1, prepend the system.cpu{idx}. to the stat name.
   */
  double getFormulaStats(const std::string &stat, int cpu = -1);
  double getScalarStats(const std::string &stat, int cpu = -1);
  double getVecStatsTotal(const std::string &stat, int cpu = -1);

  static bool isCPU(const SimObject *so, const std::string &suffix, int &cpuId);
  static int getTLBSize(const TheISA::TLB *tlb);
  static int getCacheTagAccessLatency(const BaseCacheParams *cacheParams);

  template <typename T> static const T *getParams(const SimObject *so) {
    auto params = dynamic_cast<const T *>(so->params());
    panic_if(params == nullptr, "Failed to get params from %s.",
             so->name().c_str());
    return params;
  }
};

#endif