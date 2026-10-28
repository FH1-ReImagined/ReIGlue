test_vmsum4fp128_1:
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [40000000, 40700000, 4013d70a, 40b051eb]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v4 [40000000, 40700000, 4013d70a, 40b051eb]
  #_ REGISTER_OUT v5 [41A5147B, 41A5147B, 41A5147B, 41A5147B]

test_vmsum4fp128_2:
  # Denormal output flushed to +0.0: FLT_MIN * 0.5 + 0 + 0 + 0 = denormal -> +0.0
  #_ REGISTER_IN v3 [00800000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v4 [3F000000, 00000000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [00000000, 00000000, 00000000, 00000000]

test_vmsum4fp128_3:
  # Negative denormal output flushed to -0.0: -FLT_MIN * 0.5 + 0 + 0 + 0 = neg denormal -> -0.0
  #_ REGISTER_IN v3 [80800000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v4 [3F000000, 00000000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [80000000, 80000000, 80000000, 80000000]

test_vmsum4fp128_4:
  # |(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX)|^2 overflows float32: QNaN, not infinity
  #_ REGISTER_IN v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  vmsum4fp128 v5, v3, v3
  blr
  #_ REGISTER_OUT v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  #_ REGISTER_OUT v5 [7fc00000, 7fc00000, 7fc00000, 7fc00000]

test_vmsum4fp128_5:
  # an infinite input gives infinity
  #_ REGISTER_IN v3 [7f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [7f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v4 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v5 [7f800000, 7f800000, 7f800000, 7f800000]

test_vmsum4fp128_6:
  # 1e20 * 1e20 - 1e20 * 1e20: the products overflow float32 but cancel
  #_ REGISTER_IN v3 [60ad78ec, 00000000, 60ad78ec, 3f800000]
  #_ REGISTER_IN v4 [60ad78ec, 00000000, e0ad78ec, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [60ad78ec, 00000000, 60ad78ec, 3f800000]
  #_ REGISTER_OUT v4 [60ad78ec, 00000000, e0ad78ec, 3f800000]
  #_ REGISTER_OUT v5 [3f800000, 3f800000, 3f800000, 3f800000]

test_vmsum4fp128_7:
  # 1 + 2^-24 + 2^-24 + 0 = 1 + 2^-23: float64 sums keep both halves of the last bit, which
  # float32 steps (dpps: (w + z) + (y + x)) round away to 1.0
  #_ REGISTER_IN v3 [3f800000, 33800000, 33800000, 00000000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [3f800001, 3f800001, 3f800001, 3f800001]

test_vmsum4fp128_8:
  # a denormal input is zero: 1 + 0 * FLT_MAX, not 1 + 1.18e-38 * FLT_MAX
  #_ REGISTER_IN v3 [007fffff, 3f800000, 00000000, 00000000]
  #_ REGISTER_IN v4 [7f7fffff, 3f800000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [3f800000, 3f800000, 3f800000, 3f800000]
