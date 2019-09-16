#ifndef __GEM_FORGE_INST_UTIL_HH__
#define __GEM_FORGE_INST_UTIL_HH__

/**
 * Some useful macro when implement inst functionality.
 */

#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"

#define GEM_FORGE_INST_GET_CPU() (xc->tcBase()->getCpuPtr())
#define GEM_FORGE_INST_GET_ACCEL_MANAGER()                                     \
  (GEM_FORGE_INST_GET_CPU()->getAccelManager())
#define GEM_FORGE_INST_GET_STREAM_ENGINE()                                     \
  (GEM_FORGE_INST_GET_ACCEL_MANAGER()->getStreamEngine())

#endif