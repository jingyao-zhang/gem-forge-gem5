#include "stream_history.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream history."
#endif

// #include "base/misc.hh""
#include "base/trace.hh"
#include "debug/StreamEngine.hh"
#include "proto/protoio.hh"

StreamHistory::StreamHistory(const std::string &_historyPath)
    : historyPath(_historyPath) {
  ProtoInputStream historyStream(_historyPath);
  LLVM::TDG::StreamHistory history;
  while (historyStream.read(history)) {
    this->histories.push_back(history);
  }
  this->nextConfig = this->histories.cbegin();
}

void StreamHistory::configure() {
  if (this->nextConfig == this->histories.cend()) {
    panic("Failed to read in the next history config.");
  }
  this->currentConfig = this->nextConfig;
  ++this->nextConfig;
  this->currentIdx = 0;
  this->previousAddr = 0;
}

std::pair<bool, uint64_t> StreamHistory::getNextAddr(bool &used) {
  if (this->currentIdx < this->currentConfig->history_size()) {
    const auto &entry = this->currentConfig->history(this->currentIdx);
    this->currentIdx++;
    this->previousAddr = entry.addr();
    // AdHoc.
    used = entry.used();
    return std::make_pair(entry.valid(), entry.addr());
  } else {
    // ! Make sure this is consistent with the cache stream engine.
    this->currentIdx++;
    used = false;
    return std::make_pair(false, this->previousAddr);
  }
}

const ::LLVM::TDG::StreamHistory &StreamHistory::getCurrentHistory() const {
  if (this->currentConfig == this->histories.cend()) {
    panic("Failed to read in the current history config.");
  }
  return *this->currentConfig;
}

const ::LLVM::TDG::StreamHistory &
StreamHistory::getHistoryAtInstance(uint64_t streamInstance) const {
  // ! StreamInstance starts at 1.
  if (streamInstance > this->histories.size()) {
    panic("Failed to read in the history config at instance %llu.",
          streamInstance);
  }
  const auto &history = this->histories.at(streamInstance - 1);
  if (history.history_size() == 0) {
    panic("History empty for streamInstance %llu in %s.\n", streamInstance,
          this->historyPath.c_str());
  }
  return this->histories.at(streamInstance - 1);
}

uint64_t StreamHistory::getCurrentStreamLength() const {
  return this->currentConfig->history_size();
}

uint64_t StreamHistory::getNumCacheLines() const {
  return this->currentConfig->num_cache_lines();
}

std::unique_ptr<StreamHistory::StreamHistoryAddrGenCallback>
StreamHistory::allocateCallbackAtInstance(uint64_t streamInstance) {
  return std::unique_ptr<StreamHistoryAddrGenCallback>(
      new StreamHistoryAddrGenCallback(
          this->getHistoryAtInstance(streamInstance)));
}

StreamValue StreamHistory::StreamHistoryAddrGenCallback::genAddr(
    uint64_t idx, const DynamicStreamParamV &params) {
  StreamValue ret{0};
  if (idx < this->history.history_size()) {
    ret.front() = this->history.history(idx).addr();
  } else {
    // ! Return the last address.
    ret.front() =
        this->history.history(this->history.history_size() - 1).addr();
  }
  return ret;
}