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

StreamHistory::StreamHistory(const std::string &_historyPath) {
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

uint64_t StreamHistory::getCurrentStreamLength() const {
  return this->currentConfig->history_size();
}

uint64_t StreamHistory::getNumCacheLines() const {
  return this->currentConfig->num_cache_lines();
}