#define STANDALONE_PUM_JITTER

#include "DataMoveCompiler.hh"

#include "TDFG.pb.h"
#include "config/have_protobuf.hh"
#ifndef HAVE_PROTOBUF
#error "Require protobuf to parse tensor dataflow graph."
#endif

#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

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

void shiftRhs1D() {

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

  // For now compile multiple times.
  PUMCommandVecT commands;
  const int runs = 400;
  auto startTime = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < runs; ++i) {
    commands = compiler.compile(sendPat, recvPat);
  }
  auto endTime = std::chrono::high_resolution_clock::now();

  for (const auto &cmd : commands) {
    printf(" CMD %s.\n", cmd.to_string().c_str());
  }

  std::chrono::duration<double, std::micro> compileTimeMs = endTime - startTime;
  compileTimeMs /= runs;
  printf("CompileTime %s %lfus.\n", __func__, compileTimeMs.count());
}

void shiftRhs2D() {

  // Simple 2D tile across the entire LLC.
  AffinePattern::IntVecT tileSizes = {16, 32};
  AffinePattern::IntVecT arraySizes = {2048, 2048};

  auto sendTile =
      AffinePattern::construct_canonical_tile(tileSizes, arraySizes);

  printf("SendTile %s.\n", sendTile.to_string().c_str());

  DataMoveCompiler compiler(hwConfig, sendTile);

  // Shift right by 1.
  AffinePattern::IntVecT sendStart = {0, 0};
  AffinePattern::IntVecT sendTrip = {2048, 2048 - 1};

  AffinePattern::IntVecT recvStart = {1};
  AffinePattern::IntVecT recvTrip = {2048, 2048 - 1};

  auto sendPat =
      AffinePattern::constructSubRegion(arraySizes, sendStart, sendTrip);
  auto recvPat =
      AffinePattern::constructSubRegion(arraySizes, recvStart, recvTrip);

  printf("SendPat %s.\n", sendPat.to_string().c_str());
  printf("RecvPat %s.\n", recvPat.to_string().c_str());

  // For now compile multiple times.
  PUMCommandVecT commands;
  const int runs = 400;
  auto startTime = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < runs; ++i) {
    commands = compiler.compile(sendPat, recvPat);
  }
  auto endTime = std::chrono::high_resolution_clock::now();

  for (const auto &cmd : commands) {
    printf(" CMD %s.\n", cmd.to_string().c_str());
  }

  std::chrono::duration<double, std::micro> compileTimeMs = endTime - startTime;
  compileTimeMs /= runs;
  printf(">> CompileTime %s %lf us\n", __func__, compileTimeMs.count());
}

void broadcastRhs2D() {

  // Simple 2D tile across the entire LLC.
  AffinePattern::IntVecT tileSizes = {16, 32};
  AffinePattern::IntVecT arraySizes = {2048, 2048};

  auto sendTile =
      AffinePattern::construct_canonical_tile(tileSizes, arraySizes);

  printf("SendTile %s.\n", sendTile.to_string().c_str());

  DataMoveCompiler compiler(hwConfig, sendTile);

  // Broadcast right by 1.
  auto sendPat = AffinePattern::parse("0:0:2048:2048:2048");
  auto recvPat = AffinePattern::parse("0:1:2048:2048:2048");

  printf("SendPat %s.\n", sendPat.to_string().c_str());
  printf("RecvPat %s.\n", recvPat.to_string().c_str());

  // For now compile multiple times.
  PUMCommandVecT commands;
  const int runs = 100;
  auto startTime = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < runs; ++i) {
    commands = compiler.compile(sendPat, recvPat);
  }
  auto endTime = std::chrono::high_resolution_clock::now();

  for (const auto &cmd : commands) {
    printf(" CMD %s.\n", cmd.to_string().c_str());
  }

  std::chrono::duration<double, std::micro> compileTimeMs = endTime - startTime;
  compileTimeMs /= runs;
  printf(">> CompileTime %s %lf us\n", __func__, compileTimeMs.count());
}

void compileTDFG(const ::LLVM::TDG::TDFG &tdfg) {

  std::unique_ptr<DataMoveCompiler> compiler = nullptr;

  for (auto nodeIdx = 0, numNodes = tdfg.nodes_size(); nodeIdx < numNodes;
       ++nodeIdx) {
    const auto &node = tdfg.nodes().at(nodeIdx);
    // So far we only care about MoveNode.
    if (node.has_move()) {

      // Search for the send node.
      auto sendNodeIdx = -1;
      for (const auto &edge : tdfg.edges()) {
        if (edge.src() == nodeIdx) {
          if (sendNodeIdx != -1) {
            fprintf(stderr, "Multiple operands for MoveNode?\n");
            exit(1);
          }
          sendNodeIdx = edge.dst();
        }
      }
      if (sendNodeIdx == -1) {
        fprintf(stderr, "Failed to find operand for MoveNode.\n");
        exit(1);
      }

      const auto &sendNode = tdfg.nodes().at(sendNodeIdx);

      AffinePattern sendTile(sendNode.pum_tile());
      AffinePattern sendPat(sendNode.pattern());
      AffinePattern recvTile(node.pum_tile());
      AffinePattern recvPat(node.pattern());

      if (!compiler || sendTile != compiler->tile_pattern) {
        // We need a new compiler.
        // Otherwise we can reuse it.
        compiler = m5::make_unique<DataMoveCompiler>(hwConfig, sendTile);
      }

      printf("SendTile %s.\n", sendTile.to_string().c_str());
      printf("RecvTile %s.\n", recvTile.to_string().c_str());

      printf("SendPat %s.\n", sendPat.to_string().c_str());
      printf("RecvPat %s.\n", recvPat.to_string().c_str());

      // For now compile multiple times.
      PUMCommandVecT commands;
      const int runs = 400;
      auto startTime = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < runs; ++i) {
        commands = compiler->compile(sendPat, recvPat);
      }
      auto endTime = std::chrono::high_resolution_clock::now();

      for (const auto &cmd : commands) {
        printf(" CMD %s.\n", cmd.to_string().c_str());
      }

      std::chrono::duration<double, std::micro> compileTimeMs =
          endTime - startTime;
      compileTimeMs /= runs;
      printf(">> CompileTime %s %lf us\n", __func__, compileTimeMs.count());
    }
  }
}

int main(int argc, char *argv[]) {

  if (argc == 1) {
    // shiftRhs1D();
    // shiftRhs2D();
    broadcastRhs2D();
    return 0;
  }

  if (argc != 2) {
    fprintf(stderr, "Usage: <exe> tdfg_json\n");
    exit(1);
  }

  auto jsonFn = argv[1];
  std::string json;
  {
    std::ifstream t(jsonFn);
    if (!t.is_open()) {
      fprintf(stderr, "Failed to open json tdfg %s.\n", jsonFn);
      exit(1);
    }
    std::stringstream buffer;
    buffer << t.rdbuf();
    json = buffer.str();
  }

  ::LLVM::TDG::TDFG tdfg;
  auto status = ::google::protobuf::util::JsonStringToMessage(json, &tdfg);
  if (!status.ok()) {
    fprintf(stderr, "Failed to parse json to tdfg %s.\n", jsonFn);
    exit(1);
  }

  printf("Parsed TDFG.\n");
  compileTDFG(tdfg);

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