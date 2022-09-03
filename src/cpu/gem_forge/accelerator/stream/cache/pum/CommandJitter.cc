#define STANDALONE_PUM_JITTER

#include "DataMoveCompiler.hh"

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

int main(int argc, char *argv[]) {

  PUMHWConfiguration hwConfig(array_rows, array_cols, array_per_way,
                              tree_degree, tree_leaf_bw_bytes, way_per_bank,
                              mesh_layers, mesh_rows, mesh_cols);

  // Simple 1D tile across the entire LLC.
  auto totalSize = array_cols * array_per_way * way_per_bank * mesh_cols *
                   mesh_rows * mesh_layers;
  AffinePattern::IntVecT tileSizes = {array_cols};
  AffinePattern::IntVecT arraySizes = {totalSize};

  auto sendTile =
      AffinePattern::construct_canonical_tile(tileSizes, arraySizes);

  printf("SendTile %s.\n", sendTile.to_string().c_str());

  DataMoveCompiler compiler(hwConfig, sendTile);

  // Shift right by 1.
  AffinePattern::IntVecT sendStart = {0};
  AffinePattern::IntVecT sendTrip = {totalSize - 1};

  AffinePattern::IntVecT recvStart = {1};
  AffinePattern::IntVecT recvTrip = {totalSize - 1};

  auto sendPat =
      AffinePattern::constructSubRegion(arraySizes, sendStart, sendTrip);
  auto recvPat =
      AffinePattern::constructSubRegion(arraySizes, recvStart, recvTrip);

  printf("SendPat %s.\n", sendPat.to_string().c_str());
  printf("RecvPat %s.\n", recvPat.to_string().c_str());

  auto commands = compiler.compile(sendPat, recvPat);

  for (const auto &cmd : commands) {
    printf(" CMD %s.\n", cmd.to_string().c_str());
  }

  printf("Hello from gem5.\n");
  return 0;
}

/**
 * @brief Copied from OpClass.cc but get rid of the pybind stuff.
 * TODO: Figure out a better way to solve this link problem.
 */
namespace Enums {
const char *OpClassStrings[Num_OpClass] = {
    "No_OpClass",
    "IntAlu",
    "IntMult",
    "IntDiv",
    "FloatAdd",
    "FloatCmp",
    "FloatCvt",
    "FloatMult",
    "FloatMultAcc",
    "FloatDiv",
    "FloatMisc",
    "FloatSqrt",
    "SimdAdd",
    "SimdAddAcc",
    "SimdAlu",
    "SimdCmp",
    "SimdCvt",
    "SimdMisc",
    "SimdMult",
    "SimdMultAcc",
    "SimdShift",
    "SimdShiftAcc",
    "SimdDiv",
    "SimdSqrt",
    "SimdFloatAdd",
    "SimdFloatAlu",
    "SimdFloatCmp",
    "SimdFloatCvt",
    "SimdFloatDiv",
    "SimdFloatMisc",
    "SimdFloatMult",
    "SimdFloatMultAcc",
    "SimdFloatSqrt",
    "SimdReduceAdd",
    "SimdReduceAlu",
    "SimdReduceCmp",
    "SimdFloatReduceAdd",
    "SimdFloatReduceCmp",
    "SimdAes",
    "SimdAesMix",
    "SimdSha1Hash",
    "SimdSha1Hash2",
    "SimdSha256Hash",
    "SimdSha256Hash2",
    "SimdShaSigma2",
    "SimdShaSigma3",
    "SimdPredAlu",
    "MemRead",
    "MemWrite",
    "FloatMemRead",
    "FloatMemWrite",
    "IprAccess",
    "InstPrefetch",
    "Accelerator",
};
} // namespace Enums