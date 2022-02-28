#include "PUMHWConfiguration.hh"

#include "sim/stream_nuca/stream_nuca_map.hh"

PUMHWConfiguration PUMHWConfiguration::getPUMHWConfig() {
  const auto &p = StreamNUCAMap::getCacheParams();

  auto meshLayers = 1;
  auto meshRows = StreamNUCAMap::getNumRows();
  auto meshCols = StreamNUCAMap::getNumCols();

  return PUMHWConfiguration(p.wordlines, p.bitlines, p.arrayPerWay,
                            p.arrayTreeDegree, p.arrayTreeLeafBandwidth,
                            p.assoc, meshLayers, meshRows, meshCols);
}