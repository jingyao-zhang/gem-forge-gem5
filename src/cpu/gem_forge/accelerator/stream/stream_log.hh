#ifndef __CPU_TDG_ACCELERATOR_STREAM_LOG_HH__
#define __CPU_TDG_ACCELERATOR_STREAM_LOG_HH__
/**
 * Include this in .cc and define DEBUG_TYPE
 */

#define S_MSG(S, format, args...)                                              \
  "[SE%d][%lu][%s]: " format, S->getCPUDelegator()->cpuId(), S->staticId,      \
      S->getStreamName(), ##args

#define S_DPRINTF_(X, S, format, args...) DPRINTF(X, S_MSG(S, format, ##args))
#define S_DPRINTF(S, format, args...) S_DPRINTF_(DEBUG_TYPE, S, format, ##args)
#define S_HACK(S, format, args...) hack(S_MSG(S, format, ##args))
#define S_PANIC(S, format, args...) panic(S_MSG(S, format, ##args))

#define S_FIFO_ENTRY_MSG(E, format, args...) "%s: " format, (E), ##args
#define S_FIFO_ENTRY_DPRINTF_(X, E, format, args...)                           \
  DPRINTF(X, S_FIFO_ENTRY_MSG((E), format, ##args))
#define S_FIFO_ENTRY_DPRINTF(E, format, args...)                               \
  S_FIFO_ENTRY_DPRINTF_(DEBUG_TYPE, (E), format, ##args)
#define S_FIFO_ENTRY_HACK(E, format, args...)                                  \
  hack(S_FIFO_ENTRY_MSG((E), format, ##args))
#define S_FIFO_ENTRY_PANIC(E, format, args...)                                 \
  panic(S_FIFO_ENTRY_MSG((E), format, ##args))

#define S_ELEMENT_MSG(E, format, args...)                                      \
  S_FIFO_ENTRY_MSG((E)->FIFOIdx, format, ##args)
#define S_ELEMENT_DPRINTF_(X, E, format, args...)                              \
  DPRINTF(X, S_ELEMENT_MSG(E, format, ##args))
#define S_ELEMENT_DPRINTF(E, format, args...)                                  \
  S_ELEMENT_DPRINTF_(DEBUG_TYPE, E, format, ##args)
#define S_ELEMENT_HACK(E, format, args...)                                     \
  hack(S_ELEMENT_MSG(E, format, ##args))
#define S_ELEMENT_PANIC(E, format, args...)                                    \
  panic(S_ELEMENT_MSG(E, format, ##args))

#define DYN_S_MSG(dynamicStreamId, format, args...)                            \
  "%s: " format, (dynamicStreamId), ##args
#define DYN_S_DPRINTF_(X, dynamicStreamId, format, args...)                    \
  DPRINTF(X, DYN_S_MSG((dynamicStreamId), format, ##args))
#define DYN_S_DPRINTF(dynamicStreamId, format, args...)                        \
  DYN_S_DPRINTF_(DEBUG_TYPE, (dynamicStreamId), format, ##args)
#define DYN_S_PANIC(dynamicStreamId, format, args...)                          \
  panic(DYN_S_MSG((dynamicStreamId), format, ##args))

#define SLICE_MSG(sliceId, format, args...)                                    \
  DYN_S_MSG("%s: " format, sliceId, ##args)

#define MLC_S_MSG(dynId, format, args...)                                      \
  "[MLC_SE%d]%s: " format, this->controller->getMachineID().num, dynId, ##args
#define MLC_SLICE_MSG(slice, format, args...)                                  \
  "[MLC_SE%d]%s: " format, this->controller->getMachineID().num, slice, ##args

#define MLC_S_DPRINTF_(X, dynId, format, args...)                              \
  DPRINTF(X, MLC_S_MSG(dynId, format, ##args))
#define MLC_S_DPRINTF(dynId, format, args...)                                  \
  MLC_S_DPRINTF_(DEBUG_TYPE, dynId, format, ##args)
#define MLC_S_WARN(dynId, format, args...)                                     \
  warn(MLC_S_MSG(dynId, format, ##args))
#define MLC_S_HACK(dynId, format, args...)                                     \
  hack(MLC_S_MSG(dynId, format, ##args))
#define MLC_S_PANIC(dynId, format, args...)                                    \
  this->panicDump();                                                           \
  panic(MLC_S_MSG(dynId, format, ##args))

#define MLC_SLICE_DPRINTF_(X, sliceId, format, args...)                        \
  DPRINTF(X, MLC_SLICE_MSG(sliceId, format, ##args))
#define MLC_SLICE_DPRINTF(sliceId, format, args...)                            \
  MLC_SLICE_DPRINTF_(DEBUG_TYPE, sliceId, format, ##args)
#define MLC_SLICE_WARN(sliceId, format, args...)                               \
  warn(MLC_SLICE_MSG(sliceId, format, ##args))
#define MLC_SLICE_HACK(sliceId, format, args...)                               \
  hack(MLC_SLICE_MSG(sliceId, format, ##args))
#define MLC_SLICE_PANIC(sliceId, format, args...)                              \
  this->panicDump();                                                           \
  panic(MLC_SLICE_MSG(sliceId, format, ##args))

#define LLC_S_MSG(streamId, format, args...)                                   \
  "[LLC_SE%d]%s: " format, this->curLLCBank(), (streamId), ##args
#define LLC_SLICE_MSG(sliceId, format, args...)                                \
  "[LLC_SE%d]%s: " format, this->curLLCBank(), (sliceId), ##args
#define LLC_SE_MSG(format, args...)                                            \
  "[LLC_SE%d]: " format, this->curLLCBank(), ##args
#define LLC_ELEMENT_MSG(element, format, args...)                              \
  "[LLC_SE%d]%s-%lu: " format, (element)->curLLCBank(),                        \
      (element)->dynStreamId, (element)->idx, ##args

#define LLC_S_DPRINTF_(X, streamId, format, args...)                           \
  DPRINTF(X, LLC_S_MSG(streamId, format, ##args))
#define LLC_S_DPRINTF(streamId, format, args...)                               \
  LLC_S_DPRINTF_(DEBUG_TYPE, streamId, format, ##args)
#define LLC_S_HACK(streamId, format, args...)                                  \
  hack(LLC_S_MSG(streamId, format, ##args))
#define LLC_S_PANIC(streamId, format, args...)                                 \
  panic(LLC_S_MSG(streamId, format, ##args))

#define LLC_SLICE_DPRINTF_(X, sliceId, format, args...)                        \
  DPRINTF(X, LLC_SLICE_MSG(sliceId, format, ##args))
#define LLC_SLICE_DPRINTF(sliceId, format, args...)                            \
  LLC_SLICE_DPRINTF_(DEBUG_TYPE, sliceId, format, ##args)
#define LLC_SLICE_PANIC(sliceId, format, args...)                              \
  panic(LLC_SLICE_MSG(sliceId, format, ##args))

#define LLC_SE_DPRINTF_(X, format, args...)                                    \
  DPRINTF(X, LLC_SE_MSG(format, ##args))
#define LLC_SE_DPRINTF(format, args...)                                        \
  LLC_SE_DPRINTF_(DEBUG_TYPE, format, ##args)

#define LLC_ELEMENT_DPRINTF_(X, element, format, args...)                      \
  DPRINTF(X, LLC_ELEMENT_MSG(element, format, ##args))
#define LLC_ELEMENT_DPRINTF(element, format, args...)                          \
  LLC_ELEMENT_DPRINTF_(DEBUG_TYPE, element, format, ##args)
#define LLC_ELEMENT_PANIC(element, format, args...)                            \
  panic(LLC_ELEMENT_MSG(element, format, ##args))
#endif