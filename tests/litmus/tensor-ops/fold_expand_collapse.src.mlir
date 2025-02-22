// VERIFY

func @fold_expand_collapse(%arg0 : tensor<?x4xf32>) -> tensor<?x4xf32>
{
  %0 = tensor.expand_shape %arg0 [[0, 1], [2]]
      : tensor<?x4xf32> into tensor<?x3x4xf32>
  %1 = tensor.collapse_shape %0 [[0, 1], [2]]
      : tensor<?x3x4xf32> into tensor<?x4xf32>
  return %1 : tensor<?x4xf32>
}

