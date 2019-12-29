/**
 * Include this in .cc and define DEBUG_TYPE
 */

#define S_MSG(S, format, args...)                                              \
  "[SE%d][%s]: " format, S->getCPUDelegator()->cpuId(),                        \
      S->getStreamName().c_str(), ##args

#define S_DPRINTF(S, format, args...)                                          \
  DPRINTF(DEBUG_TYPE, S_MSG(S, format, ##args))
#define S_HACK(S, format, args...) hack(S_MSG(S, format, ##args))
#define S_PANIC(S, format, args...) panic(S_MSG(S, format, ##args))

#define S_ELEMENT_MSG(E, format, args...)                                      \
  S_MSG((E)->getStream(), "[%lu, %lu]: " format,                               \
        (E)->FIFOIdx.streamId.streamInstance, (E)->FIFOIdx.entryIdx, ##args)

#define S_ELEMENT_DPRINTF(E, format, args...)                                  \
  DPRINTF(DEBUG_TYPE, S_ELEMENT_MSG(E, format, ##args))
#define S_ELEMENT_HACK(E, format, args...)                                     \
  hack(S_ELEMENT_MSG(E, format, ##args))
#define S_ELEMENT_PANIC(E, format, args...)                                    \
  panic(S_ELEMENT_MSG(E, format, ##args))

#define S_FIFO_ENTRY_MSG(E, format, args...) "%s: " format, (E), ##args
#define S_FIFO_ENTRY_DPRINTF(E, format, args...)                               \
  DPRINTF(DEBUG_TYPE, S_FIFO_ENTRY_MSG((E), format, ##args))
#define S_FIFO_ENTRY_HACK(E, format, args...)                                  \
  hack(S_FIFO_ENTRY_MSG((E), format, ##args))
#define S_FIFO_ENTRY_PANIC(E, format, args...)                                 \
  panic(S_FIFO_ENTRY_MSG((E), format, ##args))

#define DYN_S_MSG(dynamicStreamId, format, args...)                            \
  "[%lu-%lu]" format, (dynamicStreamId).staticId,                              \
      (dynamicStreamId).streamInstance, ##args
#define DYN_S_DPRINTF(dynamicStreamId, format, args...)                        \
  DPRINTF(DEBUG_TYPE, DYN_S_MSG((dynamicStreamId), format, ##args))

#define SLICE_MSG(sliceId, format, args...)                                    \
  DYN_S_MSG((sliceId).streamId, "[%lu, +%d) " format, (sliceId).lhsElementIdx,      \
            (sliceId).rhsElementIdx - (sliceId).lhsElementIdx, ##args)

#define MLC_S_MSG(format, args...)                                             \
  "[MLC_SE%d][%lu-%lu]: " format, this->controller->getMachineID().num,        \
      this->dynamicStreamId.staticId, this->dynamicStreamId.streamInstance,    \
      ##args
#define MLC_SLICE_MSG(sliceId, format, args...)                                \
  "[MLC_SE%d][%lu-%lu][%lu, +%d): " format,                                    \
      this->controller->getMachineID().num, this->dynamicStreamId.staticId,    \
      this->dynamicStreamId.streamInstance, (sliceId).lhsElementIdx,                \
      (sliceId).rhsElementIdx - (sliceId).lhsElementIdx, ##args

#define MLC_S_DPRINTF(format, args...)                                         \
  DPRINTF(DEBUG_TYPE, MLC_S_MSG(format, ##args))
#define MLC_S_PANIC(format, args...)                                           \
  this->panicDump();                                                           \
  panic(MLC_S_MSG(format, ##args))

#define MLC_SLICE_DPRINTF(sliceId, format, args...)                            \
  DPRINTF(DEBUG_TYPE, MLC_SLICE_MSG(sliceId, format, ##args))
#define MLC_SLICE_PANIC(sliceId, format, args...)                              \
  this->panicDump();                                                           \
  panic(MLC_SLICE_MSG(sliceId, format, ##args))

#define LLC_S_MSG(streamId, format, args...)                                   \
  "[LLC_SE%d][%lu-%lu]: " format, this->controller->getMachineID().num,        \
      (streamId).staticId, (streamId).streamInstance, ##args
#define LLC_SLICE_MSG(sliceId, format, args...)                                \
  "[LLC_SE%d][%lu-%lu][%lu, +%d): " format,                                    \
      this->controller->getMachineID().num, (sliceId).streamId.staticId,       \
      (sliceId).streamId.streamInstance, (sliceId).lhsElementIdx,                   \
      (sliceId).rhsElementIdx - (sliceId).lhsElementIdx, ##args

#define LLC_S_DPRINTF(streamId, format, args...)                               \
  DPRINTF(DEBUG_TYPE, LLC_S_MSG(streamId, format, ##args))

#define LLC_SLICE_DPRINTF(sliceId, format, args...)                            \
  DPRINTF(DEBUG_TYPE, LLC_SLICE_MSG(sliceId, format, ##args))
