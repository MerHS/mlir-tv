// VERIFY

func @depthwise1(%arg0: tensor<2x5x5x2xf32>, %arg1: tensor<2x2x2x3xf32>) -> tensor<2x4x4x2x3xf32> {
  %0 = linalg.init_tensor [2, 4, 4, 2, 3] : tensor<2x4x4x2x3xf32>
  %cst = arith.constant 0.000000e+00 : f32
  %1 = linalg.fill(%cst, %0) : f32, tensor<2x4x4x2x3xf32> -> tensor<2x4x4x2x3xf32> 
  %2 = linalg.depthwise_conv_2d_nhwc_hwcm {dilations = dense<1> : tensor<2xi64>, strides = dense<1> : tensor<2xi64>} ins(%arg0, %arg1 : tensor<2x5x5x2xf32>, tensor<2x2x2x3xf32>) outs(%1 : tensor<2x4x4x2x3xf32>) -> tensor<2x4x4x2x3xf32>
  return %2 : tensor<2x4x4x2x3xf32>
}

// mlir-opt -linalg-bufferize