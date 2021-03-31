#ifndef __GEM_FORGE_ACCELERATOR_STREAM_ATOMIC_OP_HH__
#define __GEM_FORGE_ACCELERATOR_STREAM_ATOMIC_OP_HH__

#include "addr_gen_callback.hh"
#include "fifo_entry_idx.hh"

class Stream;
class StreamAtomicOp : public AtomicOpFunctor {
public:
  StreamAtomicOp(Stream *_stream, const FIFOEntryIdx &_entryIdx, uint8_t _size,
                 const DynamicStreamParamV &_params,
                 const ExecFuncPtr &_storeFunc, const ExecFuncPtr &_loadFunc,
                 StreamValue _loadedValue = StreamValue())
      : stream(_stream), entryIdx(_entryIdx), size(_size), params(_params),
        storeFunc(_storeFunc), loadFunc(_loadFunc), loadedValue(_loadedValue) {
    assert(!this->params.empty() && "Should at least have one atomic operand.");
    assert(this->size <= sizeof(DynamicStreamParamV::value_type) &&
           "Illegal size.");
  }

  void operator()(uint8_t *p) override;

  AtomicOpFunctor *clone() override {
    return new StreamAtomicOp(stream, entryIdx, size, params, storeFunc,
                              loadFunc, loadedValue);
  }

  const StreamValue &getLoadedValue() const { return this->loadedValue; }
  bool modifiedMemory() const { return this->memoryModified; }

private:
  Stream *stream;
  const FIFOEntryIdx entryIdx;
  // Size of the final atomic operand.
  const uint8_t size;
  DynamicStreamParamV params;
  ExecFuncPtr storeFunc;
  ExecFuncPtr loadFunc;
  // The final loaded value back to core.
  StreamValue loadedValue;
  // Whether this atomic actually changes the content.
  bool memoryModified = false;
};

#endif