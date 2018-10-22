
#ifndef __CPU_TDG_ACCELERATOR_STREAM_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_HH__

#include "stream_history.hh"

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

  uint64_t getStreamId() const { return this->info.id(); }
  const std::string &getStreamName() const { return this->info.name(); }

  uint64_t getFirstConfigSeqNum() const { return this->firstConfigSeqNum; }
  bool isBeforeFirstConfigInst(uint64_t seqNum) const {
    if (this->firstConfigSeqNum == LLVMDynamicInst::INVALID_SEQ_NUM) {
      return true;
    }
    return seqNum < this->firstConfigSeqNum;
  }

  void configure(uint64_t configSeqNum);
  void commitConfigure(uint64_t configSeqNum);
  void step(uint64_t stepSeqNum);
  void commitStep(uint64_t stepSeqNum);
  void store(uint64_t storeSeqNum);
  void commitStore(uint64_t storeSeqNum);
  void tick();

  bool isReady(const LLVMDynamicInst *user) const;
  void use(const LLVMDynamicInst *user);
  bool canStep() const;

private:
  LLVMTraceCPU *cpu;
  StreamEngine *se;
  LLVM::TDG::StreamInfo info;
  std::unique_ptr<StreamHistory> history;

  std::unordered_set<Stream *> baseStreams;
  std::unordered_set<Stream *> baseStepStreams;
  std::unordered_set<Stream *> baseStepRootStreams;
  std::unordered_set<Stream *> dependentStreams;
  std::unordered_set<Stream *> dependentStepStreams;
  /**
   * Step the dependent streams in this order.
   */
  std::list<Stream *> stepStreamList;

  void addBaseStream(Stream *baseStream);
  void addBaseStepStream(Stream *baseStepStream);
  void registerStepDependentStreamToRoot(Stream *newDependentStream);
  bool isStepRoot() const {
    return this->baseStepStreams.empty() &&
           (this->info.type() == "phi" || this->info.type() == "store");
  }

  struct FIFOEntryIdx {
    uint64_t streamInstance;
    uint64_t configSeqNum;
    uint64_t entryIdx;
    FIFOEntryIdx()
        : streamInstance(0), configSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
          entryIdx(0) {}
    FIFOEntryIdx(uint64_t _streamInstance, uint64_t _configSeqNum,
                 uint64_t _entryIdx)
        : streamInstance(_streamInstance), configSeqNum(_configSeqNum),
          entryIdx(_entryIdx) {}
    FIFOEntryIdx(const FIFOEntryIdx &other)
        : streamInstance(other.streamInstance),
          configSeqNum(other.configSeqNum), entryIdx(other.entryIdx) {}
    void next() { this->entryIdx++; }
    void newInstance(uint64_t configSeqNum) {
      this->entryIdx = 0;
      this->streamInstance++;
      this->configSeqNum = configSeqNum;
    }

    bool operator==(const FIFOEntryIdx &other) const {
      return this->streamInstance == other.streamInstance &&
             this->entryIdx == other.entryIdx;
    }
    bool operator!=(const FIFOEntryIdx &other) const {
      return !(this->operator==(other));
    }
  };

  struct FIFOEntry {
    const FIFOEntryIdx idx;

    /**
     * For iv stream, address is the same as value.
     * TODO: Remove value, which is unused in the simulator.
     */
    const uint64_t address;
    const uint64_t value;
    bool isAddressValid;
    bool isValueValid;
    bool used;
    int inflyLoadPackets;
    /**
     * The sequence number of the step instruction.
     */
    const uint64_t prevSeqNum;
    uint64_t stepSeqNum;
    uint64_t storeSeqNum;
    Cycles addressReadyCycles;
    Cycles valueReadyCycles;
    mutable Cycles firstCheckIfReadyCycles;
    mutable std::unordered_set<uint64_t> users;

    FIFOEntry(const FIFOEntryIdx &_idx, const uint64_t _address,
              const uint64_t _prevSeqNum)
        : idx(_idx), address(_address), value(0), isAddressValid(false),
          isValueValid(false), used(false), inflyLoadPackets(0),
          prevSeqNum(_prevSeqNum), stepSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM),
          storeSeqNum(LLVMDynamicInst::INVALID_SEQ_NUM) {}

    void markAddressReady(Cycles readyCycles);
    void markValueReady(Cycles readyCycles);

    bool stored() const {
      return this->storeSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;
    }
    bool stepped() const {
      return this->stepSeqNum != LLVMDynamicInst::INVALID_SEQ_NUM;
    }
    void store(uint64_t storeSeqNum);
    void step(uint64_t stepSeqNum);
    void dump() const;
  };

  /**
   * This is used as a handler to send/receive packet from
   * cpu.
   */
  class StreamMemAccess final : public TDGPacketHandler {
  public:
    StreamMemAccess(Stream *_stream, const FIFOEntryIdx _entryId)
        : stream(_stream), entryId(_entryId) {}
    virtual ~StreamMemAccess() {}
    void handlePacketResponse(LLVMTraceCPU *cpu, PacketPtr packet) override;

  private:
    Stream *stream;
    FIFOEntryIdx entryId;
  };

  bool active;
  uint64_t firstConfigSeqNum;
  uint64_t configSeqNum;

  uint8_t *storedData;

  const size_t RUN_AHEAD_FIFO_ENTRIES;
  FIFOEntryIdx FIFOIdx;
  std::list<FIFOEntry> FIFO;

  std::unordered_set<StreamMemAccess *> memAccesses;

  mutable std::unordered_map<uint64_t, const FIFOEntry *> userToEntryMap;

  bool isMemStream() const {
    return this->info.type() == "load" || this->info.type() == "store";
  }

  void enqueueFIFO();

  bool checkIfEntryBaseValuesValid(const FIFOEntry &entry) const;

  void handlePacketResponse(const FIFOEntryIdx &entryId, PacketPtr packet,
                            StreamMemAccess *memAccess);
  void triggerReady(Stream *rootStream, const FIFOEntryIdx &entryId);
  void receiveReady(Stream *rootStream, Stream *baseStream,
                    const FIFOEntryIdx &entryId);

  void triggerStep(uint64_t stepSeqNum, Stream *rootStream);
  void stepImpl(uint64_t stepSeqNum);

  void triggerCommitStep(uint64_t stepSeqNum, Stream *rootStream);
  void commitStepImpl(uint64_t stepSeqNum);

  void markAddressReady(FIFOEntry &entry);
  void markValueReady(FIFOEntry &entry);

  /**
   * Find the correct used entry by comparing the userSeqNum and stepSeqNum of
   * the entry.
   * Returns nullptr if failed. This can happen when the last element is
   * stepped, but the FIFO is full and the next element is not allocated yet.
   */
  FIFOEntry *findCorrectUsedEntry(uint64_t userSeqNum);
  const FIFOEntry *findCorrectUsedEntry(uint64_t userSeqNum) const;

  /**
   * For debug.
   */
  void dump() const;
};

#endif