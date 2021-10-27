
#ifndef __CPU_GEM_FORGE_ACCELERATOR_STREAM_CACHE_STREAM_REUSE_BUFFER_HH__
#define __CPU_GEM_FORGE_ACCELERATOR_STREAM_CACHE_STREAM_REUSE_BUFFER_HH__

#include "../stream.hh"
#include "DynamicStreamSliceId.hh"

#include "base/types.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/common/MachineID.hh"

#include <list>
#include <unordered_map>
#include <vector>

class ReuseBuffer {
public:
  ReuseBuffer(int _maxNumLines);

  void addLine(Addr paddrLine, const DataBlock &dataBlock);
  bool contains(Addr paddrLine);
  const DataBlock &reuse(Addr paddrLine);

private:
  const int maxNumLines;

  struct CachedLine {
    Addr paddrLine;
    DataBlock dataBlock;
    CachedLine(Addr _paddrLine, const DataBlock &_dataBlock)
        : paddrLine(_paddrLine), dataBlock(_dataBlock) {}
  };

  using CachedLines = std::list<CachedLine>;
  using CachedLinesIter = CachedLines::iterator;
  CachedLines cachedLines;
  std::unordered_map<Addr, CachedLinesIter> paddrLineToCachedIterMap;
};

class StreamReuseBuffer {
public:
  StreamReuseBuffer(MachineID _machineId, int _maxNumLines, bool _perCoreMode);

  void addLine(const DynamicStreamSliceId &sliceId, Addr paddrLine,
               const DataBlock &dataBlock);
  bool contains(const DynamicStreamSliceId &reuseSliceId, Addr paddrLine);
  const DataBlock &reuse(const DynamicStreamSliceId &reuseSliceId,
                         Addr paddrLine);

  bool shouldCacheStream(Stream *S, const DynamicStreamId &dynSId) const;
  bool shouldCheckReuse(Stream *S, const DynamicStreamId &dynSId) const;

private:
  const MachineID machineId;
  const char *machineTypeStr;
  const int maxNumLines;
  const bool perCoreMode;

  /**
   * Memorized CachedStream.
   */
  mutable std::unordered_map<Stream *, Stream *>
      memorizedAliasBaseToChosenCacheStreamMap;

  /**
   * For DPRINTF.
   */
  int curRemoteBank() const;
  MachineType myMachineType() const;
  const char *curRemoteMachineType() const;

  int getCoreId(const DynamicStreamId &dynSId) const;

  using ReuseBufferPtr = std::shared_ptr<ReuseBuffer>;
  std::vector<ReuseBufferPtr> coreToReuseBuffers;

  ReuseBufferPtr &getReuseBuffer(const DynamicStreamId &dynSId);
};

#endif