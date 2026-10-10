test_vmsum3fp128_1:
  # v3 = [1.0,  1.5,  1.1, 0.0]
  # v4 = [2.0, 3.75, 2.31, 0.0]
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 01020304]
  #_ REGISTER_IN v4 [40000000, 40700000, 4013d70a, 01020304]
  vmsum3fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [3f800000, 3fc00000, 3f8ccccd, 01020304]
  #_ REGISTER_OUT v4 [40000000, 40700000, 4013d70a, 01020304]
  #_ REGISTER_OUT v5 [4122A7F0, 4122A7F0, 4122A7F0, 4122A7F0]

test_vmsum3fp128_2:
  # |(-FLT_MAX, -FLT_MAX, -FLT_MAX)|^2 overflows float32: QNaN, not infinity
  #_ REGISTER_IN v3 [ff7fffff, ff7fffff, ff7fffff, 00000000]
  vmsum3fp128 v5, v3, v3
  blr
  #_ REGISTER_OUT v3 [ff7fffff, ff7fffff, ff7fffff, 00000000]
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum3fp128_3:
  # an infinite input gives infinity
  #_ REGISTER_IN v3 [7f800000, 3f800000, 3f800000, 00000000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 00000000]
  vmsum3fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [7f800000, 3f800000, 3f800000, 00000000]
  #_ REGISTER_OUT v4 [3f800000, 3f800000, 3f800000, 00000000]
  #_ REGISTER_OUT v5 [7f800000, 7f800000, 7f800000, 7f800000]

test_vmsum3fp128_4:
  # 1e20 * 1e20 - 1e20 * 1e20: the products overflow float32 but cancel, and w is ignored
  #_ REGISTER_IN v3 [60ad78ec, 60ad78ec, 00000000, 7f800000]
  #_ REGISTER_IN v4 [60ad78ec, e0ad78ec, 00000000, 7f800000]
  vmsum3fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [60ad78ec, 60ad78ec, 00000000, 7f800000]
  #_ REGISTER_OUT v4 [60ad78ec, e0ad78ec, 00000000, 7f800000]
  #_ REGISTER_OUT v5 [00000000, 00000000, 00000000, 00000000]
