#include "encode.h"
#include "utils.h"
#include "abstractops.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Matchers.h"

#include <functional>
#include <map>
#include <sstream>
#include <variant>
#include <vector>
#include <optional>

using namespace smt;
using namespace std;



static ValueTy attrToValueTy(mlir::Attribute a) {
  auto ty = a.getType();
  if (ty.isa<mlir::FloatType>()) {
    return Float::constant(a.dyn_cast<mlir::FloatAttr>().getValue(), ty);
  } else if (ty.isa<mlir::IntegerType>()) {
    if (64 < ty.getIntOrFloatBitWidth())
      throw UnsupportedException("Integer size is too large");

    return Integer(a.dyn_cast<mlir::IntegerAttr>().getValue());
  } else if (ty.isIndex()) {
    llvm::APInt i = a.dyn_cast<mlir::IntegerAttr>().getValue();
    assert(i.getBitWidth() == 64);
    int64_t ii = i.getSExtValue();
    assert(-2147483648ll <= ii && ii <= 2147483647ll);
    return Index(ii);
  }

  throw UnsupportedException("Unsupported type");
}

static Tensor elemAttrToTensor(
    mlir::ElementsAttr attr, mlir::RankedTensorType tensorty) {

  mlir::Type elemType = tensorty.getElementType();

  if (auto denseAttr = attr.dyn_cast<mlir::DenseElementsAttr>()) {
    if (denseAttr.isSplat()) {
      // A constant tensor's type cannot have unknown dimensions
      auto dims = ShapedValue::getDims(tensorty, false);
      auto v = attrToValueTy(denseAttr.getSplatValue<mlir::Attribute>());

      return Tensor(elemType, getExpr(v), move(dims));

    } else {
      int64_t rank = tensorty.getRank();
      vector<int64_t> dims;
      vector<Expr> dimExprs;
      for (int i = 0; i < rank; ++i) {
        auto dsize = tensorty.getDimSize(i);
        assert(dsize != mlir::ShapedType::kDynamicSize);
        dims.push_back(dsize);
        dimExprs.push_back(Index(dsize));
      }

      vector<uint64_t> elems(rank);
      vector<Expr> exprs;

      while (true) {
        if (elems.back() == dims.back()) {
          int focus = rank - 1;
          while (1 <= focus && elems[focus] == dims[focus]) {
            elems[focus] = 0;
            elems[focus - 1]++;
            focus--;
          }

          if (elems[0] == dims[0])
            break;
        }

        exprs.push_back(getExpr(attrToValueTy(denseAttr.getValues<mlir::Attribute>()[elems])));
        elems.back()++;
      }

      return Tensor(elemType, move(exprs)).reshape(dimExprs);
    }

  } else if (auto sparseAttr = attr.dyn_cast<mlir::SparseElementsAttr>()) {
    auto sparseIndexValues = sparseAttr.getIndices().getValues<uint64_t>();
    auto elemTy = tensorty.getElementType();
    auto rank = tensorty.getRank();
    vector<uint64_t> dims;
    for (unsigned i = 0; i < rank; ++i)
      dims.push_back(tensorty.getDimSize(i));

    // Unspecified locations are filled with zero.
    auto zero = getZero(elemTy);
    if (!zero)
      throw UnsupportedException("unsupported element type");

    vector<vector<uint64_t>> sparseIndices;
    vector<Expr> sparseValues;

    auto sparseIndBeg = sparseIndexValues.begin();
    while (sparseIndBeg != sparseIndexValues.end()) {
      vector<uint64_t> curIndices;
      for (unsigned i = 0; i < rank; ++i) {
        curIndices.push_back(*sparseIndBeg);
        sparseIndBeg++;
      }

      auto value = sparseAttr.getValues<mlir::Attribute>()[curIndices];
      sparseIndices.push_back(move(curIndices));

      auto e = attrToValueTy(value);
      sparseValues.push_back(getExpr(e));
    }
    return Tensor(elemTy, sparseIndices, sparseValues, dims, *zero);
  }

  throw UnsupportedException("unsupported attribute");
}

static optional<ValueTy> fromExpr(Expr &&e, mlir::Type ty) {
  if (ty.isIndex())
    return Index(e);
  else if (ty.isa<mlir::FloatType>())
    return Float(e, ty);
  else if (ty.isa<mlir::IntegerType>()) {
    assert(e.sort().bitwidth() == ty.getIntOrFloatBitWidth());
    return Integer(e);
  }
  return {};
}

// map := (i, j, k) -> (j, k, i)
// input := [a, b, c]
// output := [b, c, a]
static vector<Expr> doMap(
    const vector<Expr> &input, const mlir::AffineMap &map) {
  if (map.isIdentity())
    return input;

  vector<Expr> output;
  for (unsigned i = 0; i < map.getNumResults(); ++i) {
    auto ade = map.getResult(i).dyn_cast<mlir::AffineDimExpr>();
    output.push_back(input[ade.getPosition()]);
  }
  return output;
}

template<class T>
static vector<T> vecAddElem(const vector<T> &a, const T &b) {
  vector<T> c;
  for (unsigned i = 0; i < a.size(); ++i)
    c.push_back(a[i] + b);
  return c;
}

static vector<Expr> addOne(vector<Expr> &&vec) {
  if (vec.empty())
    return {};
  return vecAddElem(vec, Expr::mkBV(1, vec[0].bitwidth()));
}

template<class T>
static vector<T> vecAdd(const vector<T> &a, const vector<T> &b) {
  assert(a.size() == b.size());
  vector<T> c;
  for (unsigned i = 0; i < a.size(); ++i)
    c.push_back(a[i] + b[i]);
  return c;
}

static Expr evalIndexCastOp(mlir::Type src, mlir::Type tgt, Expr &&val) {
  assert(val.sort().isBV());

  unsigned srcWidth = val.sort().bitwidth();

  unsigned destWidth = 0;
  if (auto dstty = tgt.dyn_cast<mlir::IntegerType>())
    destWidth = dstty.getWidth();
  else {
    assert(tgt.isIndex());
    destWidth = Index::BITS;
  }

  Expr casted = val;
  if (srcWidth > destWidth)
    casted = val.extract(destWidth - 1, 0);
  else if (srcWidth < destWidth)
    casted = val.sext(destWidth - srcWidth);
  return casted;
}

template<class ValTy>
vector<ValTy> getFromMixedOps(
    const State &st, const llvm::SmallVector<mlir::OpFoldResult> &mixedOps) {
  vector<ValTy> vec;
  for (auto s: mixedOps) {
    vec.push_back(s.is<mlir::Value>() ?
      st.regs.get<ValTy>(s.get<mlir::Value>()) :
      Index(s.get<mlir::Attribute>().dyn_cast<mlir::IntegerAttr>().getInt()));
  }
  return vec;
}



template<class T>
optional<Expr> encodeAffineExpr(
    mlir::AffineExpr ae, const vector<T> &dimvars, const vector<T> &symbolvars
) {
  switch (ae.getKind()) {
  case mlir::AffineExprKind::Add:
  case mlir::AffineExprKind::Mul: {
    auto aboe = ae.dyn_cast<mlir::AffineBinaryOpExpr>();
    auto lhs = encodeAffineExpr(aboe.getLHS(), dimvars, symbolvars);
    auto rhs = encodeAffineExpr(aboe.getRHS(), dimvars, symbolvars);
    if (!lhs || !rhs)
      return {};
    return (ae.getKind() == mlir::AffineExprKind::Add) ?
        *lhs + *rhs : *lhs * *rhs;
  }
  case mlir::AffineExprKind::DimId: {
    auto ade = ae.dyn_cast<mlir::AffineDimExpr>();
    auto id = ade.getPosition();
    assert(id < dimvars.size());
    return dimvars[id];
  }
  case mlir::AffineExprKind::SymbolId: {
    auto ade = ae.dyn_cast<mlir::AffineSymbolExpr>();
    auto id = ade.getPosition();
    assert(id < symbolvars.size());
    return symbolvars[id];
  }
  case mlir::AffineExprKind::Constant: {
    auto ac = ae.dyn_cast<mlir::AffineConstantExpr>();
    if (ac.getValue() < 0)
      return {};
    return Index(ac.getValue());
  }
  default:
    // Unsupported
    return {};
  }
}

static mlir::Type getElemTy(mlir::Value v) {
  return v.getType().dyn_cast<mlir::ShapedType>().getElementType();
}


static optional<pair<Tensor, Tensor>>
broadcastTensors(State &st, mlir::Value arg0, mlir::Value arg1) {
  // reference: https://numpy.org/doc/stable/user/basics.broadcasting.html
  auto ty0 = arg0.getType().cast<mlir::RankedTensorType>();
  auto ty1 = arg1.getType().cast<mlir::RankedTensorType>();
  auto t0 = st.regs.get<Tensor>(arg0);
  auto t1 = st.regs.get<Tensor>(arg1);
  auto ty0rank = max(ty0.getRank(), (int64_t)1);
  auto ty1rank = max(ty1.getRank(), (int64_t)1);
  auto getDimSize = [](mlir::RankedTensorType ty, int idx) -> int64_t {
    if (ty.getRank() == 0) {
      assert(idx == 0);
      return 1;
    }
    return ty.getDimSize(idx);
  };

  auto resRank = max(ty0rank, ty1rank);
  auto inVars0 = Index::boundIndexVars(resRank);
  auto inVars1 = Index::boundIndexVars(resRank);
  Expr izero = Index(0);

  vector<Expr> outVars0, outVars1;
  // The dimensions of broadcasted t0 and t1 are separately maintained (not
  // mixed). This is for a correct encoding of shape check (shape mismatch is
  // UB)
  vector<Expr> resDims0, resDims1;
  for (int64_t i = 0; i < min(ty0rank, ty1rank); i++) {
    int64_t idx0 = ty0rank - 1 - i;
    int64_t idx1 = ty1rank - 1 - i;

    auto d1 = getDimSize(ty0, idx0);
    auto d2 = getDimSize(ty1, idx1);

    bool dyn0 = d1 == mlir::ShapedType::kDynamicSize;
    bool dyn1 = d2 == mlir::ShapedType::kDynamicSize;
    if (dyn0 ^ dyn1)
      return nullopt;

    assert(d1 == 1 || d2 == 1 || d1 == d2);

    if (dyn0 && dyn1) {
      resDims0.insert(resDims0.begin(), t0.getDim(idx0));
      resDims1.insert(resDims1.begin(), t1.getDim(idx1));
    } else {
      resDims0.insert(resDims0.begin(), Index(max(d1,d2)));
      resDims1.insert(resDims1.begin(), Index(max(d1,d2)));
    }

    outVars0.insert(outVars0.begin(), d1 == 1 ? izero : inVars0[idx0]);
    outVars1.insert(outVars1.begin(), d2 == 1 ? izero : inVars1[idx1]);
  }

  if (ty0rank < ty1rank) {
    for (int64_t i = ty1rank - ty0rank - 1; i >= 0; --i) {
      auto d = t1.getDim(i);
      resDims0.insert(resDims0.begin(), d);
      resDims1.insert(resDims1.begin(), d);
      outVars1.insert(outVars1.begin(), inVars1[i]);
    }
  } else if (ty1rank < ty0rank) {
    for (int64_t i = ty0rank - ty1rank - 1; i >= 0; --i) {
      auto d = t0.getDim(i);
      resDims0.insert(resDims0.begin(), d);
      resDims1.insert(resDims1.begin(), d);
      outVars0.insert(outVars0.begin(), inVars0[i]);
    }
  }

  auto m0 = Tensor::mkLambda(t0.getElemType(), move(resDims0), move(inVars0),
                              t0.get(outVars0).first);

  auto m1 = Tensor::mkLambda(t1.getElemType(), move(resDims1), move(inVars1),
                              t1.get(outVars1).first);

  return {{m0, m1}};
}

template<class OpTy>
static void
encodeBinaryOp(State &st, OpTy op, mlir::Value arg0, mlir::Value arg1,
    function<Float(Float &&e1, Float &&e2)> f_float,
    function<Integer(Integer &&e1, Integer &&e2)> f_int) {

  mlir::Operation *opr = op.getOperation();

  if (arg0.getType().isa<mlir::FloatType>()) {
    auto a = st.regs.get<Float>(arg0);
    auto b = st.regs.get<Float>(arg1);
    st.regs.add(op, f_float(move(a), move(b)));

  } else if (auto tty = arg0.getType().dyn_cast<mlir::RankedTensorType>()) {
    auto elemty = tty.getElementType();
    if (!elemty.isIntOrFloat())
      throw UnsupportedException(opr, "Unsupported element type");

    auto bts = broadcastTensors(st, arg0, arg1);
    if (!bts)
      throw UnsupportedException(opr, "Unsupported broadcast form");
    auto [a, b] = *bts;

    auto f = [&](Expr &&a, Expr &&b) -> Expr {
      if (elemty.isa<mlir::FloatType>()) {
        return f_float(Float(a, elemty), Float(b, elemty));
      } else if (elemty.isa<mlir::IntegerType>()) {
        return f_int(Integer(a), Integer(b));
      }
      throw UnsupportedException(opr, "Unknown value type");
    };
    st.regs.add(op, a.elementwiseBinOp(b, elemty, f));
    st.wellDefined(op.getOperation(), listsEqual(a.getDims(), b.getDims()));

  } else {
    throw UnsupportedException(opr, "Unsupported type");
  }
}

template<class OpTy>
static void
encodeUnaryOp(State &st, OpTy op, mlir::Value arg,
    function<Float(Float &&e)> f_float,
    function<Integer(Integer &&e)> f_int) {

  mlir::Operation *opr = op.getOperation();

  if (arg.getType().isa<mlir::FloatType>()) {
    auto a = st.regs.get<Float>(arg);
    st.regs.add(op, f_float(move(a)));

  } else if (auto tty = arg.getType().dyn_cast<mlir::RankedTensorType>()) {
    auto elemty = tty.getElementType();
    if (!elemty.isIntOrFloat())
      throw UnsupportedException(opr, "Unsupported element type");

    auto a = st.regs.get<Tensor>(arg);

    auto f = [&](Expr &&a) -> Expr {
      if (elemty.isa<mlir::FloatType>()) {
        return f_float(Float(a, elemty));
      } else if (elemty.isa<mlir::IntegerType>()) {
        return f_int(Integer(a));
      }
      throw UnsupportedException(opr, "Unknown value type");
    };
    st.regs.add(op, a.elementwiseUnaryOp(elemty, f));

  } else {
    throw UnsupportedException(opr, "Unsupported type");
  }
}


template<class T>
static void encodeOp(State &st, T op, bool encodeMemWriteOp);

// Encode the final state after executing this block.
static void encodeBlock(
    State &st, mlir::Block &block, bool printOps, bool encodeMemWriteOps,
    // checkBeforeEnc: return true if the op is to be ignored
    function<bool(mlir::Operation *, int)> checkBeforeEnc,
    function<void(mlir::Operation *)> callbackAfterEnc);


template<>
void encodeOp(State &st, mlir::arith::AddFOp op, bool) {
  mlir::Value arg0 = op.getOperand(0);
  mlir::Value arg1 = op.getOperand(1);

  encodeBinaryOp(st, op, arg0, arg1,
      [](auto &&a, auto &&b) { return a.add(b); }, {});
}

template<>
void encodeOp(State &st, mlir::arith::MulFOp op, bool) {
  mlir::Value arg0 = op.getOperand(0);
  mlir::Value arg1 = op.getOperand(1);

  encodeBinaryOp(st, op, arg0, arg1,
      [](auto &&a, auto &&b) { return a.mul(b); }, {});
}

template<>
void encodeOp(State &st, mlir::arith::NegFOp op, bool) {
  mlir::Value arg = op.getOperand();

  encodeUnaryOp(st, op, arg,
      [](auto &&a) { return a.neg(); }, {});
}

template<>
void encodeOp(State &st, mlir::arith::SubFOp op, bool) {
  mlir::Value arg0 = op.getOperand(0);
  mlir::Value arg1 = op.getOperand(1);

  encodeBinaryOp(st, op, arg0, arg1,
      [](auto &&a, auto &&b) { return a.add(b.neg()); }, {});
}

static void addIntOrIndex(
    State &st, mlir::Value res, const Expr &e, bool isIndex) {
  if (isIndex)
    st.regs.add(res, Index(e));
  else
    st.regs.add(res, Integer(e));
}

template<>
void encodeOp(State &st, mlir::arith::AddIOp op, bool) {
  auto a = st.regs.getExpr(op.getOperand(0));
  auto b = st.regs.getExpr(op.getOperand(1));
  addIntOrIndex(st, op, a + b, op.getType().isIndex());
}

template<>
void encodeOp(State &st, mlir::arith::SubIOp op, bool) {
  auto a = st.regs.getExpr(op.getOperand(0));
  auto b = st.regs.getExpr(op.getOperand(1));
  addIntOrIndex(st, op, a - b, op.getType().isIndex());
}

template<>
void encodeOp(State &st, mlir::arith::MulIOp op, bool) {
  auto a = st.regs.getExpr(op.getOperand(0));
  auto b = st.regs.getExpr(op.getOperand(1));
  addIntOrIndex(st, op, a * b, op.getType().isIndex());
}

template<>
void encodeOp(State &st, mlir::arith::CmpFOp op, bool) {
  switch (op.predicate()) {
  case mlir::arith::CmpFPredicate::OLT: { // ordered (unsinged) less than "<"
    auto op1Type = op.getOperand(0).getType();
    auto op2Type = op.getOperand(1).getType();

    if (op1Type.isa<mlir::TensorType>() && op2Type.isa<mlir::TensorType>()) {
      auto a = st.regs.get<Tensor>(op.getOperand(0));
      auto b = st.regs.get<Tensor>(op.getOperand(1));
      assert(a.getElemType() == b.getElemType());

      auto elemty = a.getElemType();
      auto resultElemTy = getElemTy(op.getResult());
      auto f = [&](Expr &&a, Expr &&b) -> Expr {
        if (elemty.isa<mlir::FloatType>()) {
          return Float(a, elemty).fult(Float(b, elemty));
        }
        throw UnsupportedException(op.getOperation(), "cmpf only accepts floating-like elemtype");
      };
      st.regs.add(op, a.elementwiseBinOp(b, resultElemTy, f));
      st.wellDefined(op.getOperation(), listsEqual(a.getDims(), b.getDims()));
    } else if (op1Type.isa<mlir::FloatType>() && op2Type.isa<mlir::FloatType>()) {
      auto a = st.regs.get<Float>(op.getOperand(0));
      auto b = st.regs.get<Float>(op.getOperand(1));
      addIntOrIndex(st, op, a.fult(b), false);
    } else {
      throw UnsupportedException(op.getOperation(), "Unsupported cmpf operand");
    }
    break;
  }
  default:
    throw UnsupportedException(op.getOperation(), "Unsupported cmpf predicate");
  }
}

template<>
void encodeOp(State &st, mlir::arith::ConstantIndexOp op, bool) {
  st.regs.add(op, Index(op.value()));
}

template<>
void encodeOp(State &st, mlir::arith::ConstantIntOp op, bool) {
  st.regs.add(op, Integer(op.value(), op.getType().getIntOrFloatBitWidth()));
}

template<>
void encodeOp(State &st, mlir::arith::ConstantFloatOp op, bool) {
  if (Float::sort(op.getType()) == nullopt)
    throw UnsupportedException(op.getOperation(), "unsupported constant type");

  auto fp = op.value();
  st.regs.add(op, Float::constant(fp, op.getType()));
}

template<>
void encodeOp(State &st, mlir::arith::ConstantOp op, bool) {
  auto attr = op.value();
  auto ty = op.getType();

  if (ty.isa<mlir::RankedTensorType>() && attr.isa<mlir::ElementsAttr>()) {
    auto te = elemAttrToTensor(
        attr.cast<mlir::ElementsAttr>(), ty.cast<mlir::RankedTensorType>());

    if (attr.isa<mlir::SparseElementsAttr>())
      st.hasConstArray = true;

    st.regs.add(op, move(te));

  } else if (auto intAttr = attr.dyn_cast<mlir::IntegerAttr>()) {
    st.regs.add(op, attrToValueTy(intAttr));

  } else {
    throw UnsupportedException(op.getOperation(), "Unsupported constant");
  }
}

enum class FPPrecision {
  // F16,
  F32,
  F64
};

static FPPrecision getPrecision(mlir::Type &type) {
  if (type.isF16()) {
    // tgt_prec = FPPrecision::F16;
    throw UnsupportedException(type, "F16 is not supported yet");
  } else if (type.isF32()) {
    return FPPrecision::F32;
  } else if (type.isF64()) {
    return FPPrecision::F64;
  } else {
    throw UnsupportedException(type, "unsupported FP type");
  }
}

template<>
void encodeOp(State &st, mlir::arith::ExtFOp op, bool) {
  auto op_type = op.getType();
  FPPrecision tgt_prec = getPrecision(op_type);

  auto operand_type = op.getOperand().getType();
  FPPrecision src_prec = getPrecision(operand_type);

  if (src_prec == tgt_prec) {
    st.regs.add(op.getResult(), st.regs.get<Float>(op.getOperand()));
    return; // extending into identical type is a no-op
  } else if (src_prec > tgt_prec) {
    throw UnsupportedException(op.getOperation(),
      "cannot ExtF into lower precision type!");
  }

  auto arg = op.getOperand();
  encodeUnaryOp(st, op, arg, [op_type](auto &&a) { return a.extend(op_type); },
      {});
}

template<>
void encodeOp(State &st, mlir::arith::TruncFOp op, bool) {
  auto op_type = op.getType();
  FPPrecision tgt_prec = getPrecision(op_type);

  auto operand_type = op.getOperand().getType();
  FPPrecision src_prec = getPrecision(operand_type);

  if (src_prec == tgt_prec) {
    st.regs.add(op.getResult(), st.regs.get<Float>(op.getOperand()));
    return; // truncating into identical type is a no-op
  } else if (src_prec < tgt_prec) {
    throw UnsupportedException(op.getOperation(),
      "cannot TruncF into higher precision type!");
  }

  auto arg = op.getOperand();
  encodeUnaryOp(st, op, arg, [op_type](auto &&a) { return a.truncate(op_type); },
      {});
}

template<>
void encodeOp(State &st, mlir::linalg::IndexOp op, bool) {
  uint64_t i = op.dim();
  assert(i < st.linalgGenericScopes.top().indVars.size());
  Expr idxvar = st.linalgGenericScopes.top().indVars[i];
  st.regs.add(op, Index(idxvar));
}

template<>
void encodeOp(State &st, mlir::math::AbsOp op, bool) {
  auto f = st.regs.get<Float>(op.getOperand());
  st.regs.add(op.getResult(), f.abs());
}

template<>
void encodeOp(State &st, mlir::arith::IndexCastOp op, bool) {
  auto srcty = op.getOperand().getType();
  auto dstty = op.getType();

  if (auto src_tensorty = srcty.dyn_cast<mlir::TensorType>()) {
    auto dst_tensorty = dstty.dyn_cast<mlir::TensorType>();
    if (!dst_tensorty)
      throw UnsupportedException(op.getOperation(), "Unknown type");

    auto src = st.regs.get<Tensor>(op.getOperand());
    auto dst_elemty = dst_tensorty.getElementType();
    auto res = src.elementwiseUnaryOp(dst_elemty, [&](auto &&e) {
      return evalIndexCastOp(src_tensorty.getElementType(),
          dst_elemty, move(e));
    });
    st.regs.add(op, move(res));

  } else {
    auto src = st.regs.getExpr(op.getOperand());
    auto res = evalIndexCastOp(srcty, dstty, move(src));
    if (dstty.isIndex())
      st.regs.add(op, Index(res));
    else
      st.regs.add(op, Integer(res));
  }
}

template<>
void encodeOp(State &st, mlir::AffineApplyOp op, bool) {
  auto m = op.getAffineMap();
  if (m.getNumResults() != 1)
    throw UnsupportedException(
        op.getOperation(), "num results is larger than one");

  auto dimOperands = op.mapOperands().take_front(m.getNumDims());
  auto symbolOperands = op.mapOperands().take_back(m.getNumSymbols());

  vector<Index> indices, symbols;
  for (auto arg: dimOperands)
    indices.push_back(st.regs.get<Index>(arg));
  for (auto symbol: symbolOperands)
    symbols.push_back(st.regs.get<Index>(symbol));

  auto res = encodeAffineExpr(m.getResult(0), indices, symbols);
  if (!res)
    throw UnsupportedException(op.getOperation(), "unsupported affine Expr");
  st.regs.add(op, Index(move(*res)));
}

template<>
void encodeOp(State &st, mlir::ReturnOp op, bool) {
  for (unsigned i = 0; i < op.getNumOperands(); ++i)
    st.retValues.push_back(st.regs.findOrCrash(op.getOperand(i)));
}

template<>
void encodeOp(State &st, mlir::SelectOp op, bool) {
  auto condTy = op.condition().getType();
  auto trueTy = op.true_value().getType();
  auto falseTy = op.true_value().getType();

  if (trueTy.isa<mlir::TensorType>() && falseTy.isa<mlir::TensorType>()) {
    if (trueTy.isa<mlir::UnrankedTensorType>() ||
        falseTy.isa<mlir::UnrankedTensorType>())
      throw UnsupportedException(op.getOperation(), "Unsupported operands");
    // It is guaranteed by mlir's verifier that condTy cannot be unranked
    assert(!condTy.isa<mlir::UnrankedTensorType>());

    auto trueValue = st.regs.get<Tensor>(op.true_value());
    auto falseValue = st.regs.get<Tensor>(op.false_value());
    // Encoding UB is necessary to support select of tensors -> linalg.generic
    Expr welldef = listsEqual(trueValue.getDims(), falseValue.getDims());
    function<Expr(const vector<Expr>&)> condFn =
        [&](const vector<Expr> &indices) -> Expr {
      return st.regs.get<Integer>(op.condition());
    };
    if (condTy.isa<mlir::RankedTensorType>()) {
      auto condValue = st.regs.get<Tensor>(op.condition());
      // Copy condValue
      condFn = [condValue](const vector<Expr> &indices) -> Expr {
        return condValue.get(indices).first;
      };
      welldef &= listsEqual(trueValue.getDims(), condValue.getDims());
    }

    auto result = Tensor::mkIte(condFn, trueValue, falseValue);
    st.regs.add(op, result);
    st.wellDefined(op, move(welldef));

  } else if (trueTy.isa<mlir::MemRefType>() &&
             falseTy.isa<mlir::MemRefType>()) {
    if (trueTy.isa<mlir::UnrankedMemRefType>() ||
        falseTy.isa<mlir::UnrankedMemRefType>())
      throw UnsupportedException(op.getOperation(), "Unsupported operands");
    if (!condTy.isa<mlir::IntegerType>())
      throw UnsupportedException(
          op.getOperation(),
          "For MemRef operands, i1 typed condition is supported only");

    auto trueValue = st.regs.get<MemRef>(op.true_value());
    auto falseValue = st.regs.get<MemRef>(op.false_value());
    auto condValue = st.regs.get<Integer>(op.condition());
    auto result = MemRef::mkIte(condValue, trueValue, falseValue);

    st.regs.add(op, result);
    // Constrain the dimensions to be equivalent, otherwise the layout info
    // becomes bogus.
    st.wellDefined(op, listsEqual(trueValue.getDims(), falseValue.getDims()));

  } else {
    assert(trueTy.isIntOrFloat() || trueTy.isIndex());

    auto trueValue = st.regs.getExpr(op.true_value());
    auto falseValue = st.regs.getExpr(op.false_value());
    auto condValue = st.regs.get<Integer>(op.condition());
    auto isTrue = (Expr)condValue == Integer::boolTrue();
    st.regs.add(op, Expr::mkIte(isTrue, trueValue, falseValue), op.getType());
  }
}

template<>
void encodeOp(State &st, mlir::shape::ShapeOfOp op, bool) {
  if (!op.getType().isa<mlir::TensorType>())
    throw UnsupportedException(op.getOperation(), "unsupported type");

  auto tensor = op.getOperand();
  if (!tensor.getType().isa<mlir::TensorType>())
    throw UnsupportedException(op.getOperation(), "unsupported type");

  auto tt = st.regs.get<Tensor>(tensor);
  auto elemTy = getElemTy(op.getResult());
  st.regs.add(op, Tensor(elemTy, tt.getDims()));
}

template<>
void encodeOp(State &st, mlir::tosa::AbsOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  auto t = st.regs.get<Tensor>(op.getOperand());
  auto ety = dty.getElementType();
  st.regs.add(op.getResult(), t.elementwiseUnaryOp(ety, [&](auto &&e) {
    return Float(e, ety).abs();
  }));
}

template<>
void encodeOp(State &st, mlir::tosa::ConcatOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  uint64_t axis = op.axis();
  auto t = st.regs.get<Tensor>(op.getOperand(0));

  for (auto tensor: op.getOperands().drop_front()) {
    auto t2 = st.regs.get<Tensor>(tensor);
    for (unsigned i = 0; i < t2.getRank(); ++i) {
      if (i != axis)
        st.wellDefined(op.getOperation(), t.getDim(i) == t2.getDim(i));
    }

    t = t.concat(t2, axis);
  }

  st.regs.add(op.getResult(), t);
}

template<>
void encodeOp(State &st, mlir::tosa::ConstOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");
  auto eattr = op.value().dyn_cast<mlir::ElementsAttr>();
  if (!eattr)
    throw UnsupportedException(op.getOperation(), "Unsupported attribute");

  st.regs.add(op, elemAttrToTensor(eattr, dty));
  if (eattr.isa<mlir::SparseElementsAttr>())
    st.hasConstArray = true;
}

template<>
void encodeOp(State &st, mlir::tosa::ReverseOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  auto t = st.regs.get<Tensor>(op.input());
  auto axis = op.axis();

  st.regs.add(op, t.reverse(axis));
}

template<>
void encodeOp(State &st, mlir::tosa::TileOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  auto t = st.regs.get<Tensor>(op.input1());
  vector<unsigned> repeat;
  for (mlir::Attribute val: op.multiples())
    repeat.push_back(val.cast<mlir::IntegerAttr>().getValue().getSExtValue());

  st.regs.add(op, t.tile(repeat));
}

template<>
void encodeOp(State &st, mlir::tosa::BitwiseAndOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  if(!getElemTy(op.input1()).isa<mlir::IntegerType>() ||
      !getElemTy(op.input2()).isa<mlir::IntegerType>())
    throw UnsupportedException(op.getOperation(), "Unsupported element type"); 
  
  mlir::Value i1 = op.input1();
  mlir::Value i2 = op.input2();

  encodeBinaryOp(st, op, i1, i2,
      nullptr,
      [](auto &&a, auto &&b) { return (Expr)a & (Expr)b; });
}

template<>
void encodeOp(State &st, mlir::tosa::BitwiseNotOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  if(!getElemTy(op.input1()).isa<mlir::IntegerType>())
    throw UnsupportedException(op.getOperation(), "Unsupported element type");

  mlir::Value i1 = op.input1();

  encodeUnaryOp(st, op, i1,
      nullptr,
      [](auto &&a) { return ~(Expr)a; });
}

template<>
void encodeOp(State &st, mlir::tosa::BitwiseOrOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  if(!getElemTy(op.input1()).isa<mlir::IntegerType>() ||
      !getElemTy(op.input2()).isa<mlir::IntegerType>())
    throw UnsupportedException(op.getOperation(), "Unsupported element type"); 
  
  mlir::Value i1 = op.input1();
  mlir::Value i2 = op.input2();

  encodeBinaryOp(st, op, i1, i2,
      nullptr,
      [](auto &&a, auto &&b) { return (Expr)a | (Expr)b; });
}

template<>
void encodeOp(State &st, mlir::tosa::BitwiseXorOp op, bool) {
  auto dty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!dty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  if(!getElemTy(op.input1()).isa<mlir::IntegerType>() ||
      !getElemTy(op.input2()).isa<mlir::IntegerType>())
    throw UnsupportedException(op.getOperation(), "Unsupported element type");
  
  mlir::Value i1 = op.input1();
  mlir::Value i2 = op.input2();

  encodeBinaryOp(st, op, i1, i2,
      nullptr,
      [](auto &&a, auto &&b) { return (Expr)a ^ (Expr)b; });
}


template<>
void encodeOp(State &st, mlir::tensor::ExtractOp op, bool) {
  // TODO: The MLIR doc isn't explicit about what happens if indices are
  // out-of-bounds. It is currently encoded as UB.

  auto t = st.regs.get<Tensor>(op.getOperand(0));
  vector<Expr> indices;
  for (auto idx0: op.indices())
    indices.emplace_back(st.regs.get<Index>(idx0));
  if (indices.empty())
    // Deal with the zero-rank tensor case
    indices.push_back(Index(0));

  auto [elem, inbounds] = t.get(indices);
  if (auto v = fromExpr(move(elem), op.getType()))
    st.regs.add(op, move(*v));
  else
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  st.wellDefined(op.getOperation(), move(inbounds));
}


static void encodeParallelLoopBodyAndOutputs(
    State &newst, mlir::Block &block, const mlir::AffineMap &outputMap,
    optional<vector<Tensor>> &tvec_res, Expr &welldef,
    // (yielded value, ind var) -> newly mapped value
    optional<function<Expr(const Expr&, const vector<Expr>&)>>
        outputValMap = nullopt) {
  // Encode the loop body
  // TODO: deal with merging memories
  vector<mlir::Value> yieldedValues;

  encodeBlock(newst, block, /*print ops*/false, /*encode mem writes*/false,
      [&yieldedValues](mlir::Operation *op, int opindex) {
        if (auto op2 = mlir::dyn_cast<mlir::linalg::YieldOp>(op)) {
          assert(op2.getNumOperands() > 0);
          for (unsigned i = 0; i < op2.getNumOperands(); i++) {
            yieldedValues.push_back(op2.getOperand(i));
          }
          return true;
        } else if (auto op2 = mlir::dyn_cast<mlir::tensor::YieldOp>(op)) {
          yieldedValues.push_back(op2.getOperand());
          return true;
        }
        return false;
      },
      [&welldef, &newst](mlir::Operation *op) {
        welldef &= newst.isOpWellDefined(op);
      });

  auto &scope = newst.linalgGenericScopes.top();
  auto outputIndVars = doMap(scope.indVars, outputMap);
  auto tensorSz = addOne(doMap(scope.indVarUpperBounds, outputMap));

  tvec_res.emplace();
  for (unsigned i = 0; i < yieldedValues.size(); i++) {
    Expr resExpr = newst.regs.getExpr(yieldedValues[i]);
    if (outputValMap)
      resExpr = (*outputValMap)(resExpr, outputIndVars);

    tvec_res->push_back(Tensor::mkLambda(yieldedValues[i].getType(),
        vector(tensorSz), vector(outputIndVars), resExpr));
  }
}

template<class T>
static void encodeConv(State &st, T op, ShapedValue::ConvLayout clayout) {
  vector<Expr> strides, dilations;
  // TODO: The result may not fit in Index::BITS
  for (auto s: op.strides())
    strides.push_back(Index(s.getSExtValue()));
  for (auto d: op.dilations())
    dilations.push_back(Index(d.getSExtValue()));

  if (op.hasTensorSemantics()) {
    auto t_input = st.regs.get<Tensor>(op.image());
    auto t_filter = st.regs.get<Tensor>(op.filter());

    auto t_res = t_input.conv(t_filter, strides, dilations, clayout);
    st.regs.add(op.getResult(0), move(t_res));
  } else {
    auto input = st.regs.get<MemRef>(op.image());
    auto filter = st.regs.get<MemRef>(op.filter());
    auto output = st.regs.get<MemRef>(op.outputs()[0]);

    if (!output.isIdentityMap())
      throw UnsupportedException(op.getOperation(),
          "The output MemRef should have identity layout.");
    auto success = output.conv(input, filter, strides, dilations, clayout);
    st.wellDefined(op, move(success));
  }
}

template<> void
encodeOp(State &st, mlir::linalg::Conv2DNchwFchwOp op, bool encodeMemWriteOp) {
  if (!op.hasTensorSemantics() && !encodeMemWriteOp)
    throw UnsupportedException(op.getOperation());

  encodeConv(st, op, ShapedValue::ConvLayout::NCHW_FCHW);
}

template<> void
encodeOp(State &st, mlir::linalg::Conv2DNhwcHwcfOp op, bool encodeMemWriteOp) {
  if (!op.hasTensorSemantics() && !encodeMemWriteOp)
    throw UnsupportedException(op.getOperation());

  encodeConv(st, op, ShapedValue::ConvLayout::NHWC_HWCF);
}

template<>
void encodeOp(State &st, mlir::linalg::InitTensorOp op, bool) {
  auto res = op.getResult();
  auto ty = res.getType().dyn_cast<mlir::RankedTensorType>();
  if (!ty || !Tensor::isTypeSupported(ty))
    throw UnsupportedException(op.getOperation(), "Unsupported tensor type");

  vector<Expr> sizes;
  if (ty.getRank() == 0) {
    sizes.push_back(Index(1));
  } else {
    for (unsigned i = 0; i < ty.getRank(); ++i) {
      if (op.isDynamicSize(i))
        sizes.push_back(st.regs.get<Index>(op.getDynamicSize(i)));
      else
        sizes.push_back(Index(op.getStaticSize(i)));
    }
  }

  // FIXME: can we use res's name?
  static int new_var_idx = 0;
  st.regs.add(res,
      Tensor(ty.getElementType(), ("init_tensor_") + to_string(new_var_idx++),
             sizes));
}

template<>
void encodeOp(State &st, mlir::linalg::TensorCollapseShapeOp op, bool) {
  Tensor t = st.regs.get<Tensor>(op.getOperand());
  mlir::RankedTensorType resTy = op.getResultType();

  auto reassocExprs = op.getReassociationIndices();
  assert(reassocExprs.size() == (size_t)resTy.getRank());

  vector<Expr> newDims;
  if (reassocExprs.size() == 0) {
    newDims.push_back(Index(1));
  } else {
    // If the collapsed size does not match op.getResultType(), it is UB.
    for (unsigned i = 0; i < reassocExprs.size(); ++i) {
      Expr size = Index::one();
      for (auto &idx: reassocExprs[i])
        size = size * t.getDim(idx);

      if (resTy.getDimSize(i) != mlir::TensorType::kDynamicSize)
        st.wellDefined(op.getOperation(), size == resTy.getDimSize(i));
      newDims.push_back(move(size));
    }
  }

  st.wellDefined(op.getOperation(), t.get1DSize() == smt::get1DSize(newDims));
  st.regs.add(op.getResult(), t.reshape(newDims));
}

template<>
void encodeOp(State &st, mlir::linalg::TensorExpandShapeOp op, bool) {
  Tensor t = st.regs.get<Tensor>(op.getOperand());

  // The fresh variables created by ShapedValue::getDims will be ignored
  // by the for loop below.
  auto newdims = ShapedValue::getDims(op.getResultType(), true);
  auto indices = op.getReassociationIndices();

  unsigned i = 0;
  for (unsigned srci = 0; srci < indices.size(); ++srci) {
    auto &ids = indices[srci];
    auto orgdim = (Expr)t.getDim(srci);

    // Allow one '?' only.
    int unknown_dim = -1;
    int64_t const_size = 1;
    for (auto id: ids) {
      if (op.getResultType().getDimSize(id) == mlir::TensorType::kDynamicSize) {
        if (unknown_dim != -1)
          throw UnsupportedException(op.getOperation(),
              "it has more than one unknown dimension size in one group");
        unknown_dim = i;
      } else {
        const_size *= op.getResultType().getDimSize(id);
      }
      ++i;
    }

    if (unknown_dim == -1)
      // Nothing to do; it is already well-defined
      continue;

    if (Index::BITS < 64 && (size_t)const_size >= (1ull << Index::BITS))
      throw UnsupportedException(op.getOperation(),
          "tensor size is too large");

    // If the original size isn't divisible, raise UB
    st.wellDefined(op, orgdim.mod(const_size) == 0);
    newdims[unknown_dim] = orgdim.udiv(const_size); 
  }

  st.regs.add(op.getResult(), t.reshape(newdims));
}

template<>
void encodeOp(State &st, mlir::linalg::MatmulOp op, bool) {
  if (!op.hasTensorSemantics())
    throw UnsupportedException(op.getOperation(),
        "tensor semantics is supported only");

  if (op.getNumInputs() != 2 || op.getNumOutputs() != 1)
    throw UnsupportedException(op.getOperation(),
        "unsupported form");

  if (getElemTy(op.getOperand(0)) != getElemTy(op.getOperand(1)) ||
      getElemTy(op.getOperand(0)) != getElemTy(op.getResult(0)))
    throw UnsupportedException(op.getOperation(),
        "unsupported types");

  Tensor a = st.regs.get<Tensor>(op.getOperand(0));
  Tensor b = st.regs.get<Tensor>(op.getOperand(1));
  Tensor result = a.matmul(b);
  st.regs.add(op.getResult(0), Tensor(result));
}

template<>
void encodeOp(State &st, mlir::linalg::PadTensorOp op, bool) {
  auto retty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!retty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  auto &region = op.getRegion();
  if (!region.hasOneBlock())
    throw UnsupportedException(op.getOperation(), "Unsupported region");
  auto &blk = *region.getBlocks().begin();

  vector<Index> padSizeLow = getFromMixedOps<Index>(st, op.getMixedLowPad());
  vector<Index> padSizeHigh = getFromMixedOps<Index>(st, op.getMixedHighPad());

  auto sourceTensor = st.regs.get<Tensor>(op.source());
  auto newTensorSize =
      vecAdd(vecAdd(sourceTensor.getDimsAsIndices(), padSizeLow), padSizeHigh);

  State newst = st;
  auto loopUpperBound = vecAddElem(newTensorSize, Index(-1));
  newst.linalgGenericScopes.push(State::LinalgGenericScope{
      move(loopUpperBound)});
  auto &indVars = newst.linalgGenericScopes.top().indVars;
  for (int i = 0; i < blk.getNumArguments(); ++i) {
    Expr idxvar = indVars[i];
    newst.regs.add(blk.getArgument(i), Index(idxvar));
  }

  auto identityMap = mlir::AffineMap::getMultiDimIdentityMap(
      retty.getRank(), op.getContext());
  auto paddingOrSource = [&](const Expr &pad, const vector<Expr> &indvars) {
    Expr isSource = Expr::mkBool(true);
    assert(indvars.size() == padSizeLow.size() &&
           indvars.size() == padSizeHigh.size());
    vector<Expr> sourceIndices;
    for (unsigned i = 0; i < indvars.size(); ++i) {
      Expr l = padSizeLow[i];
      Expr h = padSizeLow[i] + sourceTensor.getDim(i);
      isSource &= l.ule(indvars[i]) & indvars[i].ult(h);
      sourceIndices.push_back(indvars[i] - l);
    }
    return Expr::mkIte(isSource, sourceTensor.get(sourceIndices).first, pad);
  };

  optional<vector<Tensor>> tvec_res;

  Expr welldef = Expr::mkBool(true);
  encodeParallelLoopBodyAndOutputs(newst, blk, identityMap, tvec_res, welldef,
      paddingOrSource);

  // pad_tensor has one output.
  welldef = Expr::mkForall(indVars,
      tvec_res->front().isInBounds(indVars).implies(welldef));

  newst.linalgGenericScopes.pop();

  // If pad_tensor's output dimension sizes are known, the padding sizes must
  // match
  if (retty.hasStaticShape()) {
    for (unsigned i = 0; i < retty.getRank(); ++i) {
      st.wellDefined(op.getOperation(),
          tvec_res->front().getDim(i) == retty.getDimSize(i));
    }
  }

  st.regs.add(op.getResult(), move(tvec_res->front()));
  st.wellDefined(op.getOperation(), move(welldef));
}

static pair<Expr, Expr> encodeDimOp(
    State &st, vector<Expr> &&dims, mlir::Value index) {
  auto idx = st.regs.get<Index>(index);

  auto res = dims[0];
  for (unsigned i = 1; i < dims.size(); ++i)
    res = Expr::mkIte((Expr)idx == i, dims[i], res);

  return {move(res), ((Expr)idx).ult(dims.size())};
}

template<>
void encodeOp(State &st, mlir::tensor::DimOp op, bool) {
  auto [res, wf] = encodeDimOp(
      st, st.regs.get<Tensor>(op.source()).getDims(), op.index());
  st.regs.add(op, Index(res));
  st.wellDefined(op.getOperation(), move(wf));
}

template<>
void encodeOp(State &st, mlir::tensor::CastOp op, bool) {
  auto tty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!tty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");

  auto t = st.regs.get<Tensor>(op.getOperand());
  for (size_t i = 0; i < tty.getRank(); ++i) {
    if (tty.isDynamicDim(i))
      continue;
    st.wellDefined(op.getOperation(), (Expr)t.getDim(i) == tty.getDimSize(i));
  }
  st.regs.add(op, move(t));
}

template<>
void encodeOp(State &st, mlir::tensor::InsertOp op, bool) {
  auto val = st.regs.get<Float>(op.scalar());
  auto dest = st.regs.get<Tensor>(op.dest());

  vector<Expr> indices;
  for (auto idx0: op.indices())
    indices.emplace_back(st.regs.get<Index>(idx0));

  auto [tensor, inbounds] = dest.insert(val, indices);
  st.regs.add(op, move(tensor));
  st.wellDefined(op.getOperation(), move(inbounds));
}

template<>
void encodeOp(State &st, mlir::tensor::FromElementsOp op, bool) {
  vector<Expr> elems;
  for (unsigned i = 0; i < op.getNumOperands(); ++i)
    elems.push_back(st.regs.getExpr(op.getOperand(i)));

  auto elemTy = op.getType().getElementType();
  st.regs.add(op.getResult(), Tensor(elemTy, move(elems)));
}

template<>
void encodeOp(State &st, mlir::tensor::GenerateOp op, bool) {
  auto exts = op.dynamicExtents();
  auto retty = op.getType().dyn_cast<mlir::RankedTensorType>();
  if (!retty)
    throw UnsupportedException(op.getOperation(), "Unsupported type");
  auto *blk = op.getBody();
  if (!blk)
    throw UnsupportedException(op.getOperation(), "Unsupported form");

  vector<Index> upperbound;
  {
    int j = 0;
    for (int i = 0; i < retty.getRank(); ++i) {
      auto d = retty.getDimSize(i);
      if (d == mlir::ShapedType::kDynamicSize) {
        auto newd = exts[j++];
        upperbound.push_back(st.regs.get<Index>(newd).ofs(-1));
      } else {
        upperbound.push_back(Index(d).ofs(-1));
      }
    }
  }

  optional<vector<Tensor>> tvec_res;
  Expr welldef = Expr::mkBool(true);
  {
    State newst = st;
    newst.linalgGenericScopes.push(State::LinalgGenericScope{move(upperbound)});
    for (int i = 0; i < blk->getNumArguments(); ++i) {
      Expr idxvar = newst.linalgGenericScopes.top().indVars[i];
      newst.regs.add(blk->getArgument(i), Index(idxvar));
    }

    auto identityMap = mlir::AffineMap::getMultiDimIdentityMap(
        retty.getRank(), op.getContext());

    encodeParallelLoopBodyAndOutputs(newst, *blk, identityMap,
        tvec_res, welldef);

    auto &indVars = newst.linalgGenericScopes.top().indVars;

    // linalg::generate has one result
    welldef = Expr::mkForall(indVars,
        tvec_res->front().isInBounds(indVars).implies(welldef));

    newst.linalgGenericScopes.pop();
  }

  // linalg::generate has one result
  st.regs.add(op.getResult(), move(tvec_res->front()));
  st.wellDefined(op.getOperation(), move(welldef));
}

template<>
void encodeOp(State &st, mlir::tensor::ExtractSliceOp op, bool) {
  vector<Index> offsets, sizes, strides;
  auto src = st.regs.get<Tensor>(op.getOperand(0));
  auto srcType = op.getOperand(0).getType().dyn_cast<mlir::ShapedType>();
  auto res = op.getResult();
  auto resType = res.getType().dyn_cast<mlir::ShapedType>();

  strides = getFromMixedOps<Index>(st, op.getMixedStrides());
  sizes = getFromMixedOps<Index>(st, op.getMixedSizes());
  offsets = getFromMixedOps<Index>(st, op.getMixedOffsets());

  if (offsets.size() != sizes.size() || sizes.size() != strides.size() ||
      strides.size() != (size_t)srcType.getRank())
    throw UnsupportedException(op.getOperation(), "Unsupported form");

  vector<Expr> dims;

  // push output dimensions to dims
  unsigned j = 0;
  for (unsigned i = 0; i < resType.getRank(); i++) {
    if (!resType.isDynamicDim(i) && resType.getDimSize(i) == 1) {
      dims.push_back(Index(1));
      continue;
    }

    // Find the new size.
    while (true) {
      assert(j < sizes.size());
      auto elem = op.getMixedSizes()[j];
      if (!elem.is<mlir::Attribute>())
        // Matched.
        break;
      auto szval = elem.get<mlir::Attribute>().dyn_cast<mlir::IntegerAttr>();
      if (szval.getInt() != 1)
        break;
      // Ignore the zero size, and look into the next one.
      j++;
    }
    
    // check if output tensor matches size or size is unknown
    dims.push_back(sizes[j]);
    j++;
  }

  vector<Expr> inIdxs, outIdxs;
  // indices that is going to be read from the output tensor
  inIdxs = Index::boundIndexVars(resType.getRank());

  // map the output tensor indices to source tensor indices
  unsigned idx = 0;
  for (unsigned i = 0; i < srcType.getRank(); i++) {
    uint64_t v;
    bool isDimSizeOne = idx >= resType.getRank() ||
        ((((Expr)sizes[i]).isUInt(v) && v == 1) &&
          resType.getDimSize(idx) != -1);
    outIdxs.push_back(isDimSizeOne ?
        (Expr)offsets[i] : (Expr)((inIdxs[idx++] * strides[i])) + offsets[i]);
  }
  st.regs.add(res,
      Tensor::mkLambda(src.getElemType(), move(dims), move(inIdxs),
                       src.get(outIdxs).first));
}

template<>
void encodeOp(State &st, mlir::tensor::InsertSliceOp op, bool) {
  vector<Index> offsets, sizes, strides;
  auto src = st.regs.get<Tensor>(op.getOperand(0));
  auto tgt = st.regs.get<Tensor>(op.getOperand(1));
  auto res = op.getResult();
  auto rank = op.getOperand(0).getType().dyn_cast<mlir::ShapedType>().getRank();
  if (rank != op.getOperand(1).getType().dyn_cast<mlir::ShapedType>().getRank()
      || rank != res.getType().dyn_cast<mlir::ShapedType>().getRank())
    throw UnsupportedException(op.getOperation(),
        "Unsupported tensor types of src and dest: their ranks do not match");

  strides = getFromMixedOps<Index>(st, op.getMixedStrides());
  sizes = getFromMixedOps<Index>(st, op.getMixedSizes());
  offsets = getFromMixedOps<Index>(st, op.getMixedOffsets());

  assert(offsets.size() == sizes.size() && sizes.size() == strides.size() &&
         strides.size() == rank);

  vector<Expr> indVars = Index::boundIndexVars(rank);
  vector<Expr> dims = tgt.getDims();
  vector<Expr> srcIdxs;

  Expr cond = Expr::mkBool(true);

  for (unsigned i = 0; i < rank; i++) {
    srcIdxs.push_back((indVars[i] - offsets[i]).udiv(strides[i]));
    cond &= ((indVars[i] - offsets[i]) % strides[i]).isZero() &
            (indVars[i] - offsets[i]).ult(sizes[i] * strides[i]);
  }

  // Picking the value from src1 must not be out of bounds.
  auto [srcelem, srcwb] = src.get(srcIdxs);
  auto [tgtelem, tgtwb] = tgt.get(indVars);
  Expr output = Expr::mkIte(cond, move(srcelem), move(tgtelem));

  // If tgt[indVars] is inbounds and the src[indVars] is to be chosen,
  // src[indVars] must be inbounds as well.
  st.wellDefined(op.getOperation(),
      Expr::mkForall(indVars, (tgtwb & cond).implies(srcwb)));
  st.regs.add(res, Tensor::mkLambda(
      src.getElemType(), move(dims), move(indVars), output));
}

template<>
void encodeOp(State &st, mlir::tosa::AddOp op, bool) {
  auto optys = op.getOperandTypes();
  if (!optys[0].isa<mlir::RankedTensorType>() ||
      !optys[1].isa<mlir::RankedTensorType>())
    throw UnsupportedException(op.getOperation(), "Unsupported operand types");

  mlir::Value arg0 = op.getOperand(0);
  mlir::Value arg1 = op.getOperand(1);

  encodeBinaryOp(st, op, arg0, arg1,
      [](auto &&a, auto &&b) { return a.add(b); },
      [](auto &&a, auto &&b) { return (Expr)a + (Expr)b; });
}

template<>
void encodeOp(State &st, mlir::tosa::SubOp op, bool) {
  auto optys = op.getOperandTypes();
  if (!optys[0].isa<mlir::RankedTensorType>() ||
      !optys[1].isa<mlir::RankedTensorType>())
    throw UnsupportedException(op.getOperation(), "Unsupported operand types");

  mlir::Value arg0 = op.getOperand(0);
  mlir::Value arg1 = op.getOperand(1);

  encodeBinaryOp(st, op, arg0, arg1,
      [](auto &&a, auto &&b) { return a.add(b.neg()); },
      [](auto &&a, auto &&b) { return (Expr)a - (Expr)b; });
}

template<>
void encodeOp(State &st, mlir::tosa::MulOp op, bool) {
  auto optys = op.getOperandTypes();
  if (!optys[0].isa<mlir::RankedTensorType>() ||
      !optys[1].isa<mlir::RankedTensorType>())
    throw UnsupportedException(op.getOperation(),
        "Unsupported operand types");

  if (op.shift() != 0)
    throw UnsupportedException(op.getOperation(),
        "Mul with shift is unsupported");

  mlir::Value arg0 = op.getOperand(0);
  mlir::Value arg1 = op.getOperand(1);

  encodeBinaryOp(st, op, arg0, arg1,
      [](auto &&a, auto &&b) { return a.mul(b); },
      [](auto &&a, auto &&b) { return (Expr)a * (Expr)b; });
}

template<>
void encodeOp(State &st, mlir::tosa::NegateOp op, bool) {
  auto opty = op.getOperand().getType();
  if (!opty.isa<mlir::RankedTensorType>())
    throw UnsupportedException(op.getOperation(), "Unsupported operand type");
  else if (op.quantization_info())
    throw UnsupportedException(op.getOperation(), "Quantization is unsupported");

  mlir::Value arg0 = op.getOperand();

  encodeUnaryOp(st, op, arg0,
      [](auto &&a) { return a.neg(); },
      [](auto &&a) { return Expr::mkBV(0, a.bitwidth()) - (Expr)a; });
}

template<>
void encodeOp(State &st, mlir::tosa::ReshapeOp op, bool) {
  auto t = st.regs.get<Tensor>(op.getOperand());
  auto attrs = op.new_shape();
  vector<Expr> newDims;
  mlir::Operation *oper = op.getOperation();

  for (auto a: attrs) {
    auto ia = a.cast<mlir::IntegerAttr>();
    if (ia.getInt() == -1)
      throw UnsupportedException(oper, "Dynamic shape is unsupported");
    newDims.push_back(Index(ia.getInt()));
  }
  st.wellDefined(oper, t.get1DSize() == smt::get1DSize(newDims));
  st.regs.add(op.getResult(), t.reshape(newDims));
}

static MemRef createNewLocalBlk(
    Memory *m, vector<Expr> &&dims, mlir::MemRefType memrefTy, bool writable) {
  if (!MemRef::isTypeSupported(memrefTy))
    throw UnsupportedException("unsupported element type");

  auto layout = MemRef::getLayout(memrefTy, dims);
  // Add a new local block
  auto bid = m->addLocalBlock(smt::get1DSize(dims), Expr::mkBool(writable));
  // Create MemRef which points to the newly created block
  auto memref =
      MemRef(m, memrefTy.getElementType(), bid, Index::zero(), dims,
          move(layout));

  return {move(memref)};
}

template<>
void encodeOp(State &st, mlir::memref::AllocOp op, bool) {
  auto memrefTy = op.getType().cast<mlir::MemRefType>();
  if (!memrefTy.getLayout().isIdentity())
    throw UnsupportedException(op.getOperation(),
        "unsupported memref type for alloc: it has a non-identity layout map");

  auto dsizes = op.dynamicSizes();
  vector<Expr> dszExprs;
  for (const auto &sz: dsizes) {
    dszExprs.push_back(st.regs.get<Index>(sz));
  }
  auto dims = ShapedValue::getDims(memrefTy, false, move(dszExprs));

  auto memref = createNewLocalBlk(st.m.get(), move(dims), memrefTy, true);
  st.regs.add(op, move(memref));
}

template<>
void encodeOp(State &st, mlir::memref::DimOp op, bool) {
  auto [res, wf] = encodeDimOp(
      st, st.regs.get<MemRef>(op.source()).getDims(), op.index());
  st.regs.add(op, Index(res));
  st.wellDefined(op.getOperation(), move(wf));
}

template<>
void encodeOp(State &st, mlir::memref::LoadOp op, bool) {
  // TODO: The MLIR doc isn't explicit about what happens if indices are
  // out-of-bounds. It is currently encoded as UB.
  auto m = st.regs.get<MemRef>(op.getOperand(0));
  vector<Expr> indices;
  for (auto idx0: op.indices())
    indices.emplace_back(st.regs.get<Index>(idx0));

  auto [Expr, success] = m.get(indices);
  if (auto vt = fromExpr(move(Expr), op.getType())) {
    st.regs.add(op, move(*vt));
    st.wellDefined(op.getOperation(), move(success));
  } else
    throw UnsupportedException(op.getOperation(), "unsupported type");
}

template<>
void encodeOp(State &st, mlir::memref::StoreOp op, bool encodeMemWriteOp) {
  if (!encodeMemWriteOp)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");

  // TODO: The MLIR doc isn't explicit about what happens if indices are
  // out-of-bounds. It is currently encoded as UB.
  auto m = st.regs.get<MemRef>(op.getOperand(1));
  vector<Expr> indices;
  for (auto idx0: op.indices())
    indices.emplace_back(st.regs.get<Index>(idx0));

  if (op.getOperand(0).getType().isF32()) {
    auto val = st.regs.get<Float>(op.getOperand(0));
    auto success = m.store(val, indices);
    st.wellDefined(op.getOperation(), move(success));
  } else {
    // Currently we support only f32 memory type
    throw UnsupportedException(op.getOperation(), "unsupported type");
  }
}

template<>
void encodeOp(State &st, mlir::memref::SubViewOp op, bool) {
  vector<Expr> sizes, offsets, strides;

  for (unsigned i = 0; i < op.getSourceType().getRank(); i++) {
#define ADD(vec, ee) { \
  vec.push_back(op.isDynamic ## ee(i) ? \
      st.regs.get<Index>(op.getDynamic ## ee(i)) : \
      Index(op.getStatic ## ee(i))); \
}
    ADD(offsets, Offset);
    ADD(sizes, Size);
    ADD(strides, Stride);
#undef ADD
  }
  auto src = st.regs.get<MemRef>(op.source());
  int rankDiff = op.getSourceType().getRank() - op.getType().getRank();
  assert(rankDiff >= 0); // only reducing rank is allowed

  // This reduction logic mainly from MLIR SubViewOp verify function.
  // See 'Dialect/MemRef/IR/MemRefOps.cpp'.
  auto expectedType = mlir::memref::SubViewOp::inferResultType(
      op.getSourceType(), extractFromI64ArrayAttr(op.static_offsets()),
      extractFromI64ArrayAttr(op.static_sizes()),
      extractFromI64ArrayAttr(op.static_strides()));

  auto originalShapedType = expectedType.cast<mlir::ShapedType>();
  auto candidateReducedShapedType = op.getType().cast<mlir::ShapedType>();
  auto optionalUnusedDimsMask = mlir::computeRankReductionMask(
    originalShapedType.getShape(),
    candidateReducedShapedType.getShape()
  );

  if (!optionalUnusedDimsMask.hasValue())
    throw UnsupportedException(op.getOperation(),
        "Subview result size mismatch");

  auto unusedDims = optionalUnusedDimsMask.getValue();
  auto memref = src.subview(offsets, sizes, strides, unusedDims, rankDiff);
  st.regs.add(op.getResult(), move(memref));
}

static void storeTensorTo(
    State &st, mlir::Operation *op, Tensor &&tensor, const MemRef &memref,
    mlir::MemRefType memrefTy) {
  if (memrefTy.getLayout().isIdentity()) {
    // memref with identity map
    auto success = memref.storeArray(tensor.asArray(), Index::zero(),
        tensor.get1DSize(), false);
    st.wellDefined(op, move(success));

  } else {
    // TODO: can we further optimize this if we know that memref is a
    // freshly created block?
    // We may not need to preserve the 'previous' bytes.

    vector<Expr> idxs = Index::boundIndexVars(memrefTy.getRank());
    auto [tVal, tSuccess] = tensor.get(idxs);
    auto [mVal, mSuccess] = memref.get(idxs);
    auto success = tSuccess & mSuccess;

    // TODO: clarify whether this is precondition or UB.
    st.wellDefined(op, Expr::mkForall(idxs, success.implies(mVal == tVal)));
    st.hasQuantifier = true;
  }
}

static Tensor loadTensorFrom(const MemRef &m) {
  auto dims = m.getDims();
  vector<Expr> idxs = Index::boundIndexVars(dims.size());
  auto expr = m.get(idxs).first;
  return Tensor::mkLambda(m.getElemType(), move(dims), move(idxs), expr);
}

template<>
void encodeOp(State &st, mlir::memref::BufferCastOp op, bool encodeMemWrite) {
  if (!encodeMemWrite)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");

  auto tensor = st.regs.get<Tensor>(op.getOperand());
  auto memrefTy = op.memref().getType().cast<mlir::MemRefType>();
  auto dims = tensor.getDims();

  // Create a read-only block.
  auto memref = createNewLocalBlk(st.m.get(), move(dims), memrefTy, false);
  storeTensorTo(st, op.getOperation(), move(tensor), memref, memrefTy);
  st.regs.add(op.memref(), move(memref));
}

template<>
void encodeOp(State &st, mlir::memref::CloneOp op, bool encodeMemWrite) {
  if (!encodeMemWrite)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");

  auto src = st.regs.get<MemRef>(op.getOperand());
  auto srcTy = op.getOperand().getType().cast<mlir::MemRefType>();
  auto dims = src.getDims();

  // Create a read-only block.
  auto memref = createNewLocalBlk(st.m.get(), move(dims), srcTy, false);
  auto tensor = loadTensorFrom(src);
  storeTensorTo(st, op.getOperation(), move(tensor), memref, srcTy);
  // Src is not writable as well.
  st.m->setWritable(src.getBID(), false);
  st.regs.add(op, move(memref));
}

template<>
void encodeOp(State &st, mlir::memref::TensorLoadOp op, bool encodeMemWrite) {
  auto m = st.regs.get<MemRef>(op.getOperand());
  // Mark the MemBlock pointed by the memref as read-only.
  auto &memory = *(st.m);
  memory.setWritable(m.getBID(), false);

  st.regs.add(op.getResult(), loadTensorFrom(m));
  st.wellDefined(op.getOperation(), m.isInBounds());
}

template<>
void encodeOp(State &st, mlir::memref::TensorStoreOp op, bool encodeMemWrite) {
  if (!encodeMemWrite)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");

  auto t = st.regs.get<Tensor>(op.tensor());
  auto m = st.regs.get<MemRef>(op.memref());

  // Src and tgt's shapes & element types must match
  // Memref may have its layout, though.
  for (unsigned i = 0; i < t.getRank(); ++i)
    st.wellDefined(op.getOperation(), (Expr)t.getDim(i) == (Expr)m.getDim(i));

  storeTensorTo(st, op.getOperation(), move(t), m,
      op.memref().getType().cast<mlir::MemRefType>());
}

template<>
void encodeOp(State &st, mlir::linalg::CopyOp op, bool encodeMemWrite) {
  if (!encodeMemWrite)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");
  else if (op.inputPermutation() || op.outputPermutation())
    // Well, this might be straightforward...
    throw UnsupportedException("linalg.copy with permutations is not supported");

  auto *opr = op.getOperation();
  auto mrIn = st.regs.get<MemRef>(op.input());
  auto mrOut = st.regs.get<MemRef>(op.output());

  // Src and tgt's shapes & element types must match
  for (unsigned i = 0; i < mrIn.getRank(); ++i)
    st.wellDefined(opr, (Expr)mrIn.getDim(i) == (Expr)mrOut.getDim(i));

  // They must not overlap, according to
  // https://mlir.llvm.org/docs/Dialects/Linalg/#linalgcopy-mlirlinalgcopyop
  st.wellDefined(opr, mrIn.noalias(mrOut));

  storeTensorTo(st, opr, loadTensorFrom(mrIn), mrOut,
      op.output().getType().cast<mlir::MemRefType>());
}

template<>
void encodeOp(State &st, mlir::linalg::FillOp op, bool encodeMemWrite) {
  if (op.hasBufferSemantics() && !encodeMemWrite)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");
  if (op.getNumResults() > 1)
    throw UnsupportedException(op.getOperation(),
        "it has multiple results");

  auto elemval = st.regs.getExpr(op.getOperand(0));
  auto op1 = op.getOperand(1);
  auto ety = getElemTy(op1);

  if (op.hasTensorSemantics()) {
    auto t = st.regs.get<Tensor>(op1);
    auto filled = Tensor(ety, move(elemval), t.getDims());
    st.regs.add(op.getResult(0), move(filled));
  } else {
    assert(op.hasBufferSemantics());
    auto m = st.regs.get<MemRef>(op1);
    auto filled = Tensor(ety, move(elemval), m.getDims());
    storeTensorTo(st, op.getOperation(), move(filled), m,
        op1.getType().cast<mlir::MemRefType>());
  }
}

template<>
void encodeOp(State &st, mlir::linalg::DotOp op, bool encodeMemWrite) {
  if (!op.hasTensorSemantics())
    throw UnsupportedException(op.getOperation(),
        "tensor semantics is supported only");

  if (op.getNumResults() != 1)
    throw UnsupportedException(op.getOperation(),
        "it has multiple results");

  auto inputOps = op.getInputOperands();
  auto outputTy = op.getType(0).dyn_cast<mlir::TensorType>();

  auto outputDim = ShapedValue::getDims(outputTy, false);
  if (outputDim.size() != 1)
    throw UnsupportedException(op.getOperation(),
        "unknown dot format; shouldn't the result tensor have one element?");

  if (outputTy.getElementType() !=
      inputOps[0]->get().getType().dyn_cast<mlir::TensorType>()
          .getElementType())
    throw UnsupportedException(op.getOperation(), "casting is not supported");

  auto t1 = st.regs.get<Tensor>(inputOps[0]->get());
  auto t2 = st.regs.get<Tensor>(inputOps[1]->get());
  st.wellDefined(op.getOperation(), t1.get1DSize() == t2.get1DSize());

  auto res = t1.dot(t2);
  st.regs.add(op.getResult(0),
      Tensor(t1.getElemType(), move(res), move(outputDim)));
}

template<>
void encodeOp(State &st, mlir::shape::ToExtentTensorOp op, bool) {
  // TODO: MLIR doc says
  //   If the shape represents an error, this op’s behavior is undefined.
  // Should figure out whether this applies to a Tensor operand as well.
  if (!op.getOperand().getType().isa<mlir::TensorType>())
    throw UnsupportedException(op.getOperation(), "unsupported type");

  auto tt = st.regs.get<Tensor>(op.getOperand());
  assert(tt.getDims().size() ==
         (size_t)op.getType().cast<mlir::TensorType>().getRank());
  st.regs.add(op, tt);
}

template<>
void encodeOp(State &st, mlir::sparse_tensor::ConvertOp op, bool) {
  auto tensor = op.getOperand();
  auto tt = st.regs.get<Tensor>(tensor);
  st.regs.add(op, move(tt));
}

vector<Index> findLoopBounds(State &st, mlir::linalg::GenericOp op) {
  // The size of the loop is calculated (analogous to what
  // LinalgOp::createLoopRanges does).
  // The process of getting the size of the loop seems fishy;
  // LinalgOp::createLoopRanges relies on the "first" dimension that is
  // matched, and it isn't clear what happens if there are multiple matching
  // dimensions. For example,
  //   linalg.generic {
  //      indexing_maps = [affine_map<(n) -> (n)>,
  //                       affine_map<(n) -> (n)>,
  //                       affine_map<(n) -> (n)>] }
  //      ins(%A, %B: <?xf32>, <?xf32>) outs(%C: <?xf32>) { .. }
  // The size of the loop is either %A, %B, or %C's dimension, but the current
  // algorithm mandates the result to be %A's dimension.

  vector<Index> viewSizes;
  for (auto *opOperand : op.getInputAndOutputOperands()) {
    unsigned r = op.getRank(opOperand);
    if (!r)
      continue;

    if (opOperand->get().getType().isa<mlir::TensorType>()) {
      auto t = st.regs.get<Tensor>(opOperand->get());
      for (int64_t i = 0, e = r; i < e; ++i) {
        viewSizes.push_back(t.getDim(i));
      }
    } else if (opOperand->get().getType().isa<mlir::MemRefType>()) {
      auto t = st.regs.get<MemRef>(opOperand->get());
      for (int64_t i = 0, e = r; i < e; ++i) {
        viewSizes.push_back(t.getDim(i));
      }
    }
  }

  if (viewSizes.empty()) {
    // Return [0] if all operands have zero rank, because there exists only
    // one element.
    // This is consistent with what ShapedValue::getDims does.
    return {Index(0)};
  }

  mlir::AffineMap map = op.getLoopsToShapesMap();
  // numDims: # of induction variables
  unsigned numDims = map.getNumDims();
  // numRes: # of output affine Exprs
  // For example, given two affine maps
  //   (i, j, k) -> (i, j)
  //   (i, j, k) -> (i, k)
  //   numDims = 3 (i, j, k), numRes = 4 (i, j, i, k)
  unsigned numRes = map.getNumResults();

  vector<Index> res;
  vector<int> resFilled(numDims);
  fill(resFilled.begin(), resFilled.end(), -1);

  for (unsigned idx = 0; idx < numRes; ++idx) {
    auto result = map.getResult(idx);
    auto d = result.dyn_cast<mlir::AffineDimExpr>();
    if (!d)
      continue;

    unsigned pos = d.getPosition();
    if (resFilled[pos] != -1)
      continue;
    // If i < N, store N - 1
    // It is to bound e.g., 'i + j <= N - 1 + M - 1'
    resFilled[pos] = res.size();
    res.push_back(viewSizes[idx].ofs(-1));
  }

  vector<Index> res_ordered;
  for (unsigned i = 0; i < numDims; ++i)
    res_ordered.push_back(move(res[resFilled[i]]));

  return res_ordered;
}

static void
encodeUBForTensorShapeMatch(State &st, mlir::linalg::GenericOp op,
                            const vector<Index> &indVarBounds) {
  mlir::AffineMap map = op.getLoopsToShapesMap();
  unsigned numRes = map.getNumResults();

  vector<Index> viewSizes;
  for (auto *opOperand : op.getInputAndOutputOperands()) {
    unsigned r = op.getRank(opOperand);
    if (!r)
      continue;

    auto value = st.regs.findOrCrash(opOperand->get());
    ShapedValue *t;
    if (holds_alternative<MemRef>(value)) {
      t = &get<MemRef>(value);
    } else if(holds_alternative<Tensor>(value)) {
      t = &get<Tensor>(value);
    } else {
      throw UnsupportedException(op.getOperation(), "Unsupported ShapedValue");
    }
    for (int64_t i = 0, e = r; i < e; ++i) {
      viewSizes.push_back(t->getDim(i));
    }
  }

  for (unsigned idx = 0; idx < numRes; ++idx) {
    auto ae = encodeAffineExpr(map.getResult(idx), indVarBounds, {});
    if (!ae)
      throw UnsupportedException(op.getOperation(), "unsupported affine Expr");

    Expr size = (Expr)viewSizes[idx];
    Expr inbounds = size.isNonZero().implies(ae->ult(size));
    st.wellDefined(op.getOperation(), move(inbounds));
  }
}

static void initInputStateForLoopBody(
    State &st, mlir::linalg::GenericOp op, Expr &welldef,
    bool isParallelLoop) {
  auto indexingMaps = op.indexing_maps().getValue();
  auto &block = *op.region().begin();

  const vector<Expr> &inductionVars = st.linalgGenericScopes.top().indVars;

  assert(op.getInputOperands().size() + op.getNumOutputs() ==
         indexingMaps.size());
  assert(op.getNumInputs() == op.getInputOperands().size());

  // The output variables contain the initial value of the tensor
  //   (see github issue #164)
  // For parallel loops: whole iterations contain the initial value
  // For reduction loops: only the first iteration contains the value
  size_t upperbound = op.getNumInputs() + op.getNumOutputs();
  for (size_t arg_i = 0; arg_i < upperbound; ++arg_i) {
    auto indexMap = indexingMaps[arg_i].cast<mlir::AffineMapAttr>().getValue();
    mlir::Value op_i = arg_i >= op.getNumInputs() ?
        op.getOutputOperand(arg_i - op.getNumInputs())->get() :
        op.getInputOperand(arg_i)->get();

    if (op_i.getType().isa<mlir::FloatType>()) {
      // A scalar value.
      Float f_input = st.regs.get<Float>(op_i);
      st.regs.add(block.getArgument(arg_i), f_input);

    } else if (auto tensorty = op_i.getType().dyn_cast<mlir::TensorType>()) {
      // A tensor value.
      auto elemty = tensorty.getElementType();
      Tensor t_input = st.regs.get<Tensor>(op_i);

      if (indexMap.getNumResults() == 0) {
        // A tensor with a single element; e.g. tensor<f32>.
        st.regs.add(block.getArgument(arg_i), t_input.get({Index::zero()}).first, elemty);
      } else {
        vector<Expr> affine_Exprs;
        for (unsigned i = 0; i < indexMap.getNumResults(); ++i) {
          auto ae_res =
              encodeAffineExpr(indexMap.getResult(i), inductionVars, {});
          if (!ae_res) {
            string msg;
            TO_STRING(msg, "Unsupported affine expr: "<< indexMap.getResult(i));
            throw UnsupportedException(op.getOperation(), move(msg));
          }

          affine_Exprs.emplace_back(move(*ae_res));
        }

        // The out-of-bounds checking is done when encoding loop bounds.
        auto t_elem = t_input.get(affine_Exprs).first;
        st.regs.add(block.getArgument(arg_i), t_elem, elemty);
      }
    } else if (auto memrefty = op_i.getType().dyn_cast<mlir::MemRefType>()) {
      // A MemRef value.
      // TODO: currently we support float32 element type
      MemRef m_input = st.regs.get<MemRef>(op_i);

      vector<Expr> affine_Exprs;
      for (unsigned i = 0; i < indexMap.getNumResults(); ++i) {
        auto ae_res =
            encodeAffineExpr(indexMap.getResult(i), inductionVars, {});
        if (!ae_res) {
          string msg;
          TO_STRING(msg, "Unsupported affine expr: "<< indexMap.getResult(i));
          throw UnsupportedException(op.getOperation(), move(msg));
        }

        affine_Exprs.emplace_back(move(*ae_res));
      }

      auto [m_elem, m_welldef] = m_input.get(affine_Exprs);
      welldef &= m_welldef;
      st.regs.add(block.getArgument(arg_i), 
          Float(m_elem, memrefty.getElementType()));
    } else {
      throw UnsupportedException(op.getOperation(),
          "unsupported block argument type");
    }
  }
}

static void encodeReductionLoopBodyAndOutput(
    State &newst, mlir::Block &block,
    const mlir::ArrayRef<mlir::Attribute> &indexingMaps,
    const mlir::ShapedType &outputType,
    optional<Tensor> &t_res,
    Expr &welldef) {
  // Deal with simple reduction loops.
  // TODO: support more kinds of reduction loops!
  string errmsg = "permutated output map or simple reduction form is"
                  " supported only";
  mlir::Operation *the_op = block.getParentOp();

  auto &ops = block.getOperations();
  int instcount = ops.size();
  mlir::Value yieldedValue;

  using mlir::m_Op;
  using mlir::matchers::m_Any;
  using mlir::matchers::m_Val;
  // Support this form:
  //   ...
  //   %sum = op %v, %arg_out or  %sum = op %arg_out, %v
  //      where op = addf, addi
  //   yield %sum
  auto lastarg = block.getArgument(block.getNumArguments() - 1);

  auto p1 = m_Op<mlir::linalg::YieldOp>(
      m_Op<mlir::arith::AddFOp>(m_Val(lastarg), m_Any()));
  auto p2 = m_Op<mlir::linalg::YieldOp>(
      m_Op<mlir::arith::AddFOp>(m_Any(), m_Val(lastarg)));
  auto p3 = m_Op<mlir::linalg::YieldOp>(
      m_Op<mlir::arith::AddIOp>(m_Val(lastarg), m_Any()));
  auto p4 = m_Op<mlir::linalg::YieldOp>(
      m_Op<mlir::arith::AddIOp>(m_Any(), m_Val(lastarg)));

  unsigned idx;
  if (p1.match(&ops.back()) || p3.match(&ops.back()))      idx = 1;
  else if (p2.match(&ops.back()) || p4.match(&ops.back())) idx = 0;
  else
    throw UnsupportedException(the_op, move(errmsg));

  auto sumvar = ops.back().getOperand(0).getDefiningOp()->getOperand(idx);

  // TODO: deal with merging memories
  encodeBlock(newst, block, /*print ops*/false, /*encode mem writes*/false,
      [&yieldedValue, instcount, &lastarg, &the_op](
          mlir::Operation *op, int opindex) {
        if (opindex >= instcount - 2)
          // Don't directly encode %sum and yield
          return true;

        auto op_operands = op->getOperands();
        for (const auto &opop: op_operands) {
          if (lastarg == opop) {
            string msg;
            TO_STRING(msg, "Unsupported reduction form because it contains "
                << *op);
            throw UnsupportedException(the_op, move(msg));
          }
        }

        return false;
      },
      [&welldef, &newst](mlir::Operation *op) {
        welldef &= newst.isOpWellDefined(op);
      });

  auto outputMap = indexingMaps.back().cast<mlir::AffineMapAttr>().getValue();

  auto &linalgInfo = newst.linalgGenericScopes.top();

  // Represent %v as an element of a tensor.
  Tensor t_v = Tensor::mkLambda(
      sumvar.getType(),
      addOne(vector(linalgInfo.indVarUpperBounds)),
      vector(linalgInfo.indVars),
      newst.regs.getExpr(sumvar));

  if (llvm::all_of(outputMap.getResults(), [](const mlir::AffineExpr &Expr) {
    auto ac = Expr.dyn_cast<mlir::AffineConstantExpr>();
    return ac && ac.getValue() == 0;
  })) {
    // in:  (i, j) -> (i, j)
    // out: (i, j) -> (0)
    // =>
    // t_res[0] = sum(\i. t_input[i / n][i % n] , i < m * n)

    // Define this as a splat tensor (num. elems is 1 anyway)
    t_res = Tensor(t_v.getElemType(), t_v.sum(),
          makeCube(Index(1), outputType.getRank()));
  } else {
    // in:  (i, j) -> (i, j)
    // out: (i, j) -> (i)
    // =>
    // t_res[i] = sum(\j. t_input[i][j] , j < m)

    // Gather affine vars that are unused in the output (e.g. j) first.
    vector<bool> isInputIdxUsed(outputMap.getNumInputs());
    for (unsigned j = 0; j < outputMap.getNumResults(); ++j) {
      auto Expr = outputMap.getResult(j);

      if (auto ade = Expr.dyn_cast<mlir::AffineDimExpr>()) {
        isInputIdxUsed[ade.getPosition()] = true;
      } else {
        // Output map has an unknown form
        throw UnsupportedException(the_op, move(errmsg));
      }
    }

    vector<Expr> boundsForRes;
    vector<Expr> indVarsForRes;
    for (unsigned j = 0; j < isInputIdxUsed.size(); ++j) {
      if (!isInputIdxUsed[j]) {
        boundsForRes.push_back(linalgInfo.indVarUpperBounds[j]);
        indVarsForRes.push_back(linalgInfo.indVars[j]);
      }
    }

    auto tensorSz = addOne(doMap(linalgInfo.indVarUpperBounds, outputMap));
    auto t_sum = Tensor::mkLambda(
          t_v.getElemType(),
          addOne(move(boundsForRes)),
          move(indVarsForRes),
          t_v.get(linalgInfo.indVars).first)
        .sum();

    auto outputIndVars = doMap(linalgInfo.indVars, outputMap);
    t_res = Tensor::mkLambda(
        t_v.getElemType(), move(tensorSz), move(outputIndVars), t_sum);
  }
}

template<>
void encodeOp(State &st, mlir::linalg::GenericOp op, bool encodeMemWriteOp) {
  if (!(op.hasTensorSemantics() || op.hasBufferSemantics()))
    throw UnsupportedException(op.getOperation(),
        "tensor/buffer semantics is supported only");

  else if (op.hasBufferSemantics() && !encodeMemWriteOp)
    throw UnsupportedException(op.getOperation(),
        "We do not support memory writes in this scope");

  auto &region = op.region();
  if (!llvm::hasSingleElement(region))
    throw UnsupportedException(op.getOperation(),
        "a single block is supported only");

  auto &block = region.front();
  if (!std::all_of(block.args_begin(), block.args_end(),
      [](auto &arg) { return arg.getType().isSignlessIntOrFloat(); }))
    throw UnsupportedException(op.getOperation(),
        "unsupported block arguments");

  if (llvm::any_of(op.iterator_types(), [](mlir::Attribute attr) {
    auto str = attr.cast<mlir::StringAttr>().getValue();
    return str != mlir::getParallelIteratorTypeName() &&
           str != mlir::getReductionIteratorTypeName() &&
           str != mlir::getWindowIteratorTypeName();
  }))
    throw UnsupportedException(op.getOperation(),
        "unsupported iterator type");

  // Find the inclusive upper bounds
  auto loopBounds = findLoopBounds(st, op);

  encodeUBForTensorShapeMatch(st, op, loopBounds);

  // Start from newst
  optional<vector<Tensor>> tvec_res;
  optional<Expr> t_welldef;
  {
    Expr welldef = Expr::mkBool(true);
    State newst = st;
    newst.linalgGenericScopes.push(State::LinalgGenericScope{loopBounds});

    auto indexingMaps = op.indexing_maps().getValue();
    auto outputMap = indexingMaps.back().cast<mlir::AffineMapAttr>().getValue();
    bool isParallelLoop = outputMap.isPermutation();

    initInputStateForLoopBody(newst, op, welldef, isParallelLoop);

    auto &indVars = newst.linalgGenericScopes.top().indVars;

    if (isParallelLoop) {
      encodeParallelLoopBodyAndOutputs(newst, block, outputMap,
          tvec_res, welldef);

    } else {
      // Reduction loops returning multiple values is not supported by MLIR-TV
      // yet.
      if (op.getNumOutputs() > 1)
        throw UnsupportedException(op.getOperation(),
            "unsupported reduction form");

      optional<Tensor> t_res;
      auto outputType = op.getOutputOperand(0)->get().getType()
          .cast<mlir::ShapedType>();
      encodeReductionLoopBodyAndOutput(newst, block,
            indexingMaps, outputType, t_res, welldef);
      tvec_res = {*t_res};
    }

    for(unsigned i = 0; i < tvec_res->size(); i++) {
      assert(tvec_res->at(i).getDims().size() != 0);
    }

    // Encode UB of linalg.generic.
    // For all induction vars' values, there must be no UB.
    Expr inbounds = Expr::mkBool(true);
    for (int i = 0; i < indVars.size(); ++i) {
      inbounds &= indVars[i].ult(loopBounds[i] + 1);
    }
    t_welldef = Expr::mkForall(indVars, inbounds.implies(welldef));
  }


  st.wellDefined(op.getOperation(), move(*t_welldef));

  if (op.hasTensorSemantics()) {
    for(unsigned i = 0; i < tvec_res->size(); i++) {
      // NOTE: op's output tensor (op.getOutputOperand()[0]->get())
      // isn't updated;
      // aqjune talked with mlir people and confirmed
      st.regs.add(op.getResult(i), move(tvec_res->at(i)));
    }
  } else if (op.hasBufferSemantics()) {
    auto success = Expr::mkBool(true);
    for(unsigned i = 0; i < tvec_res->size(); i++) {
      auto m_res = st.regs.get<MemRef>(op.getOutputOperand(i)->get());
      success &= m_res.storeArray(tvec_res->at(i).asArray(), Index::zero(),
          tvec_res->at(i).get1DSize());
    }
    st.wellDefined(op, move(success));
  } else {
    llvm_unreachable("Unknown linalg::genric semantics");
  }
}


#define ENCODE(st, op, ty, encodeMemWriteOps) \
  if (auto op2 = mlir::dyn_cast<ty>(op)) { \
    encodeOp(st, op2, encodeMemWriteOps); \
    if (callbackAfterEnc) callbackAfterEnc(&op); \
    continue; \
  }

static void encodeBlock(
    State &st, mlir::Block &block, bool printOps, bool encodeMemWriteOps,
    // checkBeforeEnc: return true if the op is to be ignored (e.g. yield)
    function<bool(mlir::Operation *, int)> checkBeforeEnc,
    function<void(mlir::Operation *)> callbackAfterEnc) {

  int index = -1;
  for (auto &op: block) {
    index++;
    if (printOps)
      llvm::outs() << "  " << op << "\n";

    if (checkBeforeEnc && checkBeforeEnc(&op, index)) continue;

    ENCODE(st, op, mlir::AffineApplyOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::SelectOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::ReturnOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::arith::AddFOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::AddIOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::CmpFOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::ConstantFloatOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::ConstantIndexOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::ConstantIntOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::ConstantOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::ExtFOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::IndexCastOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::MulFOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::MulIOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::NegFOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::SubFOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::SubIOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::arith::TruncFOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::math::AbsOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::memref::AllocOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::BufferCastOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::CloneOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::DimOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::LoadOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::StoreOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::SubViewOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::TensorLoadOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::memref::TensorStoreOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::linalg::Conv2DNchwFchwOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::Conv2DNhwcHwcfOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::CopyOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::DotOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::FillOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::GenericOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::IndexOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::InitTensorOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::MatmulOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::PadTensorOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::TensorCollapseShapeOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::linalg::TensorExpandShapeOp, encodeMemWriteOps);
    
    ENCODE(st, op, mlir::shape::ShapeOfOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::shape::ToExtentTensorOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::sparse_tensor::ConvertOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::tensor::CastOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::DimOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::InsertOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::ExtractOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::ExtractSliceOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::FromElementsOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::GenerateOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tensor::InsertSliceOp, encodeMemWriteOps);

    ENCODE(st, op, mlir::tosa::AbsOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::AddOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::BitwiseAndOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::BitwiseNotOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::BitwiseOrOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::BitwiseXorOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::ConcatOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::ConstOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::MulOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::NegateOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::ReshapeOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::ReverseOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::SubOp, encodeMemWriteOps);
    ENCODE(st, op, mlir::tosa::TileOp, encodeMemWriteOps);

    throw UnsupportedException(&op);
  }
  if (printOps)
    llvm::outs() << "\n";
}

void encode(State &st, mlir::FuncOp &fn, bool printOps) {
  auto &region = fn.getRegion();
  if (!llvm::hasSingleElement(region))
    throw UnsupportedException(
        region.getParentOp(), "Only a region with one block is supported");

  auto &block = region.front();

  encodeBlock(st, block, printOps, true/*allow mem ops*/, {}, {});
}
