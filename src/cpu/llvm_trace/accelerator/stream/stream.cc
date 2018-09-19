#include "stream.hh"

#include "base/misc.hh"
#include "base/trace.hh"
#include "debug/StreamEngine.hh"

Stream::Stream(const LLVM::TDG::TDGInstruction &configInst) {
  if (!configInst.has_stream_config()) {
    panic("Stream constructor with non stream config instruction.");
  }

  const auto &streamName = configInst.stream_config().stream_name();
  const auto &streamId = configInst.stream_config().stream_id();
  const auto &infoPath = configInst.stream_config().info_path();
  ProtoInputStream infoIStream(infoPath);
  if (!infoIStream.read(this->info)) {
    panic("Failed to read in the stream info for stream %s from file %s.",
          streamName.c_str(), infoPath.c_str());
  }

  if (this->info.name() != streamName) {
    panic("Mismatch of stream name from stream config instruction (%s) and "
          "info file (%s).",
          streamName.c_str(), this->info.name().c_str());
  }
  if (this->info.id() != streamId) {
    panic("Mismatch of stream id from stream config instruction (%lu) and "
          "info file (%lu).",
          streamId, this->info.id());
  }
  DPRINTF(StreamEngine, "Initialized stream %s.\n", this->info.name().c_str());
}
