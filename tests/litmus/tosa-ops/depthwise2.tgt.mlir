func @conv() -> tensor<1x1x1x2xf32> {
  %0 = arith.constant dense<[[[[1.0,2.0]]]]> : tensor<1x1x1x2xf32>
  return %0 : tensor<1x1x1x2xf32>
}