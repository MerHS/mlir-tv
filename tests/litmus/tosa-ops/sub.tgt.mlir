func @add(%arg0: tensor<10x10x10xf32>, %arg1: tensor<10x10x10xf32>) -> tensor<10x10x10xf32> {
  %arg1_neg = "tosa.negate"(%arg1) : (tensor<10x10x10xf32>) -> tensor<10x10x10xf32>
  %0 = "tosa.add"(%arg0, %arg1_neg) : (tensor<10x10x10xf32>, tensor<10x10x10xf32>) -> tensor<10x10x10xf32>
  return %0 : tensor<10x10x10xf32>
}

