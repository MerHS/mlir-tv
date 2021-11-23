// VERIFY

func @const() -> tensor<3xi32> {
    %c1 = arith.constant 1 : i32
    %c2 = arith.constant 2 : i32
    %c3 = arith.constant 3 : i32
    %i1 = arith.constant 0 : index
    %i2 = arith.constant 1 : index
    %i3 = arith.constant 2 : index
    %1 = linalg.init_tensor [3] : tensor<3xi32>
    %2 = tensor.insert %c1 into %1[%i1] : tensor<3xi32>
    %3 = tensor.insert %c2 into %2[%i2] : tensor<3xi32>
    %4 = tensor.insert %c3 into %3[%i3] : tensor<3xi32>
    return %4 : tensor<3xi32>
}