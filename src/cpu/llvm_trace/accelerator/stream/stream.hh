#ifndef __CPU_TDG_ACCELERATOR_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_HH__

#include "stream_pattern.hh"

// Parse the instructions from a protobuf.
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse stream info."
#endif

#include "cpu/llvm_trace/accelerator/stream/StreamMessage.pb.h"
#include "cpu/llvm_trace/llvm_insts.hh"

#include "base/types.hh"
#include "mem/packet.hh"

#include <list>

class LLVMTraceCPU;

class StreamEngine;
class Stream {
public:
  Stream(const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst,
         LLVMTraceCPU *_cpu, StreamEngine *_se);

  ~Stream();

  void addBaseStream(Stream *baseStream);

  uint64_t getStreamId() const { return this->info.id(); }
  const std::string &getStreamName() const { return this->info.name(); }

  void configure();
  void step(uint64_t stepSeqNum);
  void commitStep(uint64_t stepSeqNum);
  void store(uint64_t userSeqNum);
  void tick();

  bool isReady(uint64_t userSeqNum) const;
  bool canStep() const;

private:
  LLVMTraceCPU *cpu;
  StreamEngine *se;
  LLVM::TDG::StreamInfo info;
  StreamPattern pattern;

  std::unordered_set<Stream *> baseStreams;
  std::unordered_set<Stream *> dependentStreams;

  struct FIFOEntry {
    const uint64_t idx;
    const uint64_t value;
    bool valid;
    bool stored;
    /**
     * The sequence number of the step instruction.
     */
    uint64_t stepSeqNum;
    Cycles readyCycles;
    Cycles firstUseCycles;

    FIFOEntry(uint64_t _idx, const uint64_t _value)
        : idx(_idx), value(_value), valid(false), stored(false),
          stepSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM) {}

    void markReady(Cycles _readyCycles) {
      this->valid = true;
      this->readyCycles = _readyCycles;
    }

    void store();

    void step(uint64_t stepSeqNum);
  };

  /**
   * This is used as a dummy dynamic instruction to send/receive packet from
   * cpu.
   */
  class StreamMemAccessInst : public LLVMDynamicInst {
  public:
    StreamMemAccessInst(Stream *_stream, uint64_t _entryId)
        : LLVMDynamicInst(dummyTDGInstruction, 1), stream(_stream),
          entryId(_entryId) {}
    void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;
    void execute(LLVMTraceCPU *cpu) override {
      panic("Calling execute() on StreamMemAccessInst.\n");
    }
    bool isCompleted() const override {
      panic("Calling isCompleted() on StreamMemAccessInst.\n");
    }

  private:
    Stream *stream;
    uint64_t entryId;
    static LLVM::TDG::TDGInstruction dummyTDGInstruction;
  };

  bool active;

  uint8_t *storedData;

  const size_t RUN_AHEAD_FIFO_ENTRIES;
  uint64_t FIFOIdx;
  std::list<FIFOEntry> FIFO;

  std::unordered_set<StreamMemAccessInst *> memInsts;

  void enqueueFIFO();
  void handlePacketResponse(uint64_t entryId, PacketPtr packet,
                            StreamMemAccessInst *memInst);
  void triggerReady(Stream *rootStream, uint64_t entryId);
  void receiveReady(Stream *rootStream, Stream *baseStream, uint64_t entryId);

  void triggerStep(uint64_t stepSeqNum, Stream *rootStream);
  void receiveStep(uint64_t stepSeqNum, Stream *rootStream, Stream *baseStream);
  void stepImpl(uint64_t stepSeqNum);

  void triggerCommitStep(uint64_t stepSeqNum, Stream *rootStream);
  void receiveCommitStep(uint64_t stepSeqNum, Stream *rootStream,
                         Stream *baseStream);
  void commitStepImpl(uint64_t stepSeqNum);

  /**
   * Find the correct used entry by comparing the userSeqNum and stepSeqNum of
   * the entry.
   */
  FIFOEntry &findCorrectUsedEntry(uint64_t userSeqNum);
  const FIFOEntry &findCorrectUsedEntry(uint64_t userSeqNum) const;
};

#endif