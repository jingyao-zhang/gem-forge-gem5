/**
 * Include this in .cc and define DEBUG_TYPE
 */

#define S_MSG(S, format, args...)                                              \
  "[SE%d][%s]: " format, S->getCPUDelegator()->cpuId(),                        \
      S->getStreamName().c_str(), ##args

#define S_DPINTF(S, format, args...)                                           \
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
