func @conv(%img: tensor<1x16x16x4xf32>, %filtr: tensor<16x3x3x4xf32>) -> tensor<1x14x14x16xf32> {
    %c0 = arith.constant 0.0 : f32
    %bias = tensor.from_elements %c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0,%c0: tensor<16xf32>
    %0 = "tosa.conv2d"(%img, %filtr, %bias) {dilation = [1, 1], pad = [0, 0, 0, 0], stride = [1, 1]} : (tensor<1x16x16x4xf32>, tensor<16x3x3x4xf32>, tensor<16xf32>) -> tensor<1x14x14x16xf32>
    return %0 : tensor<1x14x14x16xf32>
}
