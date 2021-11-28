#pragma once

#include "llvm/ADT/DenseSet.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "utils.h"
#include <map>
#include <optional>
#include <set>

struct FPAnalysisResult {
  std::set<llvm::APFloat> constSet;
  size_t argCount = 0;
  size_t varCount = 0;
};

struct MemRefAnalysisResult {
  TypeMap<size_t> argCount;
  TypeMap<size_t> varCount;
  std::map<std::string, mlir::memref::GlobalOp> usedGlobals;
};

struct AnalysisResult {
  FPAnalysisResult F32;
  FPAnalysisResult F64;
  MemRefAnalysisResult memref;
};

AnalysisResult analyze(mlir::FuncOp &fn, bool isFullyAbstract);
