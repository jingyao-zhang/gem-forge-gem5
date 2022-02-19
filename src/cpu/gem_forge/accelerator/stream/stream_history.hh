#ifndef __CPU_TDG_ACCELERATOR_STREAM_HISTORY_H__
#define __CPU_TDG_ACCELERATOR_STREAM_HISTORY_H__

#include "StreamMessage.pb.h"

#include "addr_gen_callback.hh"

#include <list>
#include <string>

class StreamHistory {
public:
  StreamHistory(const std::string &_historyPath);

  /**
   * Read the next history entry from the stream.
   */
  void configure();

  const ::LLVM::TDG::StreamHistory &getCurrentHistory() const;
  const ::LLVM::TDG::StreamHistory &
  getHistoryAtInstance(uint64_t streamInstance) const;

  struct StreamHistoryAddrGenCallback : public AddrGenCallback {
    StreamValue genAddr(uint64_t idx,
                        const DynStreamParamV &params) override;
    const ::LLVM::TDG::StreamHistory &history;
    StreamHistoryAddrGenCallback(const ::LLVM::TDG::StreamHistory &_history)
        : history(_history) {}
    Cycles getEstimatedLatency() const override { return Cycles(1); }
  };

  std::unique_ptr<StreamHistoryAddrGenCallback>
  allocateCallbackAtInstance(uint64_t streamInstance);

  /**
   * Return the next value of the history.
   * The first boolean indicating the value is valid.
   */
  std::pair<bool, uint64_t> getNextAddr(bool &used);

  uint64_t getCurrentStreamLength() const;

  uint64_t getNumCacheLines() const;

private:
  std::string historyPath;
  using HistoryList = std::vector<LLVM::TDG::StreamHistory>;
  HistoryList histories;

  HistoryList::const_iterator nextConfig;
  HistoryList::const_iterator currentConfig;

  size_t currentIdx;
  uint64_t previousAddr;
};

#endif