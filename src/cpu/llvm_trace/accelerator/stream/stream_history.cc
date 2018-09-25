#include "stream_history.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"

StreamHistory::StreamHistory(const std::string &_historyPath)
    : historyPath(_historyPath), historyStream(_historyPath) {}

void StreamHistory::configure() {
  if (!this->historyStream.read(this->history)) {
    panic("Failed to read in the next history from file %s.",
          this->historyPath.c_str());
  }
  this->currentIdx = 0;
  this->previousAddr = 0;
}

std::pair<bool, uint64_t> StreamHistory::getNextAddr() {
  if (this->currentIdx < this->history.history_size()) {
    const auto &entry = this->history.history(this->currentIdx);
    this->currentIdx++;
    this->previousAddr = entry.addr();
    return std::make_pair(entry.valid(), entry.addr());
  } else {
    this->currentIdx++;
    return std::make_pair(false, this->previousAddr);
  }
}