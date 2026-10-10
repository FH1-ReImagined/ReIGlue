test_vmsum4fp128_1:
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [40000000, 40700000, 4013d70a, 40b051eb]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v4 [40000000, 40700000, 4013d70a, 40b051eb]
  #_ REGISTER_OUT v5 [41A5147B, 41A5147B, 41A5147B, 41A5147B]

test_vmsum4fp128_2:
  # |(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX)|^2 overflows float32: QNaN, not infinity
  #_ REGISTER_IN v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  vmsum4fp128 v5, v3, v3
  blr
  #_ REGISTER_OUT v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_3:
  # an infinite input gives infinity
  #_ REGISTER_IN v3 [7f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [7f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v4 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v5 [7f800000, 7f800000, 7f800000, 7f800000]

test_vmsum4fp128_4:
  # 1e20 * 1e20 - 1e20 * 1e20: the products overflow float32 but cancel
  #_ REGISTER_IN v3 [60ad78ec, 00000000, 60ad78ec, 3f800000]
  #_ REGISTER_IN v4 [60ad78ec, 00000000, e0ad78ec, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [60ad78ec, 00000000, 60ad78ec, 3f800000]
  #_ REGISTER_OUT v4 [60ad78ec, 00000000, e0ad78ec, 3f800000]
  #_ REGISTER_OUT v5 [3f800000, 3f800000, 3f800000, 3f800000]
