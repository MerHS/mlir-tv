// VERIFY-INCORRECT

// This is incorrect if arg0 is NaN or +-Inf

func @f(%arg0: f64) -> f64 {
  %neg = arith.constant -1.0 : f64
  %arg0_neg = arith.mulf %arg0, %neg : f64
  %c = arith.addf %arg0, %arg0_neg : f64
  return %c : f64
}
