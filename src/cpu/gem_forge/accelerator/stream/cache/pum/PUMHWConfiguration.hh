#ifndef __CPU_GEM_FORGE_PUM_HW_CONFIGURATION_HH__
#define __CPU_GEM_FORGE_PUM_HW_CONFIGURATION_HH__

#include "base/types.hh"

#include <cstdlib>
#include <tuple>

class PUMHWConfiguration {

public:
  /**
    This represents a mesh topology LLC configuration (from bottom up):
    Each SRAM array is organized as A (wordline) x B (bitline).
    Each way contains L SRAM arrays are connected with a tree of degree D and
    leaf bandwidth S.
    Each LLC bank contains W ways. Each chip contains M x M LLC banks.

    Way:
     -----------------------------------------------------------------
                                                                                            |
    |   <-----------------------    L    ---------------------->      |
    |       B           B                                   B         |
    |     -----       -----                               -----       |
    |   A|     |    A|     |          ...               A|     |      |
    |    |     |     |     |                             |     |      |
    |     -----       -----                               -----       |
    |     S |    D    S |                            D    S |         |
    |        -----------                          ----------          |
    |             |                                   |               |
                             ...            ...
     -----------------------------------------------------------------

   */

  const int64_t array_rows;
  const int64_t array_cols;
  const int64_t array_per_way;
  const int64_t tree_degree;
  const int64_t tree_leaf_bw_bytes;
  const int64_t way_per_bank;
  const int64_t mesh_layers;
  const int64_t mesh_rows;
  const int64_t mesh_cols;

  static PUMHWConfiguration getPUMHWConfig();

  PUMHWConfiguration(int64_t _array_rows, int64_t _array_cols,
                     int64_t _array_per_way, int64_t _tree_degree,
                     int64_t _tree_leaf_bw_bytes, int64_t _way_per_bank,
                     int64_t _mesh_layers, int64_t _mesh_rows,
                     int64_t _mesh_cols)
      : array_rows(_array_rows), array_cols(_array_cols),
        array_per_way(_array_per_way), tree_degree(_tree_degree),
        tree_leaf_bw_bytes(_tree_leaf_bw_bytes), way_per_bank(_way_per_bank),
        mesh_layers(_mesh_layers), mesh_rows(_mesh_rows),
        mesh_cols(_mesh_cols) {}

  int64_t get_total_arrays() const {
    return mesh_rows * mesh_cols * mesh_layers * way_per_bank * array_per_way;
  }

  int64_t get_total_banks() const {
    return mesh_layers * mesh_rows * mesh_cols;
  }

  int64_t get_array_per_bank() const { return way_per_bank * array_per_way; }

  int64_t get_bitlines_per_bank() const {
    return get_array_per_bank() * array_cols;
  }

  int64_t get_array_idx_from_bitline_idx(int64_t bitline_idx) const {
    return bitline_idx / array_cols;
  }

  int64_t get_way_leaf_idx_from_array_idx(int64_t array_idx) const {
    return array_idx % array_per_way;
  }

  int64_t get_way_idx_from_array_idx(int64_t array_idx) const {
    return array_idx / array_per_way;
  }

  int64_t get_bank_idx_from_way_idx(int64_t way_idx) const {
    return way_idx / way_per_bank;
  }

  int64_t get_bank_idx_from_array_idx(int64_t array_idx) const {
    return get_bank_idx_from_way_idx(get_way_idx_from_array_idx(array_idx));
  }

  int64_t get_hops_between_bank(int64_t bank_idx1, int64_t bank_idx2) const {
    auto ret1 = get_mesh_layer_row_col_from_bank_idx(bank_idx1);
    auto layer1 = std::get<0>(ret1);
    auto row1 = std::get<1>(ret1);
    auto col1 = std::get<2>(ret1);
    auto ret2 = get_mesh_layer_row_col_from_bank_idx(bank_idx2);
    auto layer2 = std::get<0>(ret2);
    auto row2 = std::get<1>(ret2);
    auto col2 = std::get<2>(ret2);
    return std::abs(layer1 - layer2) + std::abs(row1 - row2) +
           std::abs(col1 - col2);
  }

  std::tuple<int64_t, int64_t, int64_t>
  get_mesh_layer_row_col_from_bank_idx(int64_t bank_idx) const {
    auto layer = bank_idx / (mesh_rows * mesh_cols);
    bank_idx = bank_idx % (mesh_rows * mesh_cols);
    return std::make_tuple<int64_t, int64_t, int64_t>(
        std::move(layer), bank_idx / mesh_cols, bank_idx % mesh_cols);
  }

  int64_t get_bank_idx_from_mesh_layer_row_col(int64_t mesh_layer,
                                               int64_t mesh_row,
                                               int64_t mesh_col) const {
    return mesh_layer * mesh_rows * mesh_cols + mesh_row * mesh_cols + mesh_col;
  }
};

#endif