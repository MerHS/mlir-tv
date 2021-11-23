func @const() -> tensor<3xi32> {
    %1 = "tosa.const"() {value = dense<[0, 1, 2]> : tensor<3xi32>} : () -> tensor<3xi32>
    return %1 : tensor<3xi32>
}