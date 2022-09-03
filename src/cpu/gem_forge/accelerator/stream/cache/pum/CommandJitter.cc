#define STANDALONE_PUM_JITTER

#include "PUMHWConfiguration.hh"
#include "AffinePattern.hh"

#include <cstdio>

const int64_t array_rows = 512;
const int64_t array_cols = 512;
const int64_t array_per_way = 8;
const int64_t tree_degree = 2;
const int64_t tree_leaf_bw_bytes = 1;
const int64_t way_per_bank = 16;
const int64_t mesh_layers = 1;
const int64_t mesh_rows = 8;
const int64_t mesh_cols = 8;

PUMHWConfiguration hwConfig(array_rows, array_cols, array_per_way, tree_degree,
                            tree_leaf_bw_bytes, way_per_bank, mesh_layers,
                            mesh_rows, mesh_cols);

int main(int argc, char *argv[]) {

  printf("Hello from gem5.\n");
  return 0;
}