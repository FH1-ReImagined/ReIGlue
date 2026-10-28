# Cases for the rules of ReIGlue 0400 and 0401 that xenia-canary's recorded cases do not reach:
# invalid operations (the default QNaN 0x7FC00000), infinities, signed zeros, and the NaN order
# and operand roles of the VMX128 forms (the order by role is carried over from vmaddfp's
# recorded cases). Expected values from the rules that match all of xenia-canary's recorded
# vmaddfp, vnmsubfp, vmaxfp and vminfp cases; the ones that need one rounding are in
# ../fma_cases.txt.

test_vmaddfp_special_1:
  # inf * 0 + 1 and inf * 1 - inf are invalid: 7fc00000 (x86 makes ffc00000);
  # -inf * -1 + 1 = +inf; -0 * +0 + -0 = -0
  #_ REGISTER_IN v3 [7f800000, 7f800000, ff800000, 80000000]
  #_ REGISTER_IN v4 [00000000, 3f800000, bf800000, 00000000]
  #_ REGISTER_IN v5 [3f800000, ff800000, 3f800000, 80000000]
  vmaddfp v6, v3, v4, v5
  blr
  #_ REGISTER_OUT v6 [7fc00000, 7fc00000, 7f800000, 80000000]
  #_ REGISTER_OUT v3 [7f800000, 7f800000, ff800000, 80000000]
  #_ REGISTER_OUT v4 [00000000, 3f800000, bf800000, 00000000]
  #_ REGISTER_OUT v5 [3f800000, ff800000, 3f800000, 80000000]

test_vmaddfp_special_2:
  # NaNs: the addend's before an invalid product; vB (addend) before vC; vA first;
  # 2 * 3 - 6 = +0
  #_ REGISTER_IN v3 [7f800000, 3f800000, ffc0000b, 40000000]
  #_ REGISTER_IN v4 [00000000, ff800001, 7fc0000c, 40400000]
  #_ REGISTER_IN v5 [7fc00009, 7fc0000a, 3f800000, c0c00000]
  vmaddfp v6, v3, v4, v5
  blr
  #_ REGISTER_OUT v6 [7fc00009, 7fc0000a, ffc0000b, 00000000]
  #_ REGISTER_OUT v3 [7f800000, 3f800000, ffc0000b, 40000000]
  #_ REGISTER_OUT v4 [00000000, ff800001, 7fc0000c, 40400000]
  #_ REGISTER_OUT v5 [7fc00009, 7fc0000a, 3f800000, c0c00000]

test_vmaddfp_special_3:
  # one rounding: FLT_MAX * 2 - FLT_MAX = FLT_MAX and FLT_MAX * 2 - inf = -inf
  # (a rounded product overflows: +inf, NaN); -0 * -0 + -0 = +0
  #_ REGISTER_IN v3 [7f7fffff, 7f7fffff, 3f800000, 80000000]
  #_ REGISTER_IN v4 [40000000, 40000000, 3f800000, 80000000]
  #_ REGISTER_IN v5 [ff7fffff, ff800000, 3f800000, 80000000]
  vmaddfp v6, v3, v4, v5
  blr
  #_ REGISTER_OUT v6 [7f7fffff, ff800000, 40000000, 00000000]
  #_ REGISTER_OUT v3 [7f7fffff, 7f7fffff, 3f800000, 80000000]
  #_ REGISTER_OUT v4 [40000000, 40000000, 3f800000, 80000000]
  #_ REGISTER_OUT v5 [ff7fffff, ff800000, 3f800000, 80000000]
