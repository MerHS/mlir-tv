#pragma once

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include <optional>
#include <set>

struct FPAnalysisResult {
  std::set<llvm::APFloat> constSet;
  size_t argCount = 0;
  size_t varCount = 0;
};

struct MemRefAnalysisResult {
  size_t argCount = 0;
  size_t varCount = 0;
};

struct AnalysisResult {
  FPAnalysisResult F32;
  FPAnalysisResult F64;
  MemRefAnalysisResult memref;
};

AnalysisResult analyze(mlir::FuncOp &fn, bool isFullyAbstract);
