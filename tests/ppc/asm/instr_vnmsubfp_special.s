# Cases for the rules of ReIGlue 0400 and 0401 that xenia-canary's recorded cases do not reach:
# invalid operations (the default QNaN 0x7FC00000), infinities, signed zeros, and the NaN order
# and operand roles of the VMX128 forms (the order by role is carried over from vmaddfp's
# recorded cases). Expected values from the rules that match all of xenia-canary's recorded
# vmaddfp, vnmsubfp, vmaxfp and vminfp cases; the ones that need one rounding are in
# ../fma_cases.txt.

test_vnmsubfp_special_1:
  # -(inf * 0 - 1) and -(inf * 1 - inf) are invalid: 7fc00000, not negated;
  # -(inf * 1 - 1) = -inf; -(+0 * +0 - +0) = -0
  #_ REGISTER_IN v3 [7f800000, 7f800000, 7f800000, 00000000]
  #_ REGISTER_IN v4 [00000000, 3f800000, 3f800000, 00000000]
  #_ REGISTER_IN v5 [3f800000, 7f800000, 3f800000, 00000000]
  vnmsubfp v6, v3, v4, v5
  blr
  #_ REGISTER_OUT v6 [7fc00000, 7fc00000, ff800000, 80000000]
  #_ REGISTER_OUT v3 [7f800000, 7f800000, 7f800000, 00000000]
  #_ REGISTER_OUT v4 [00000000, 3f800000, 3f800000, 00000000]
  #_ REGISTER_OUT v5 [3f800000, 7f800000, 3f800000, 00000000]

test_vnmsubfp_special_2:
  # NaNs pass with their sign: vA first, then vB (the addend), then vC;
  # -(-0 * +0 - +0) = +0
  #_ REGISTER_IN v3 [7fc00001, 3f800000, 3f800000, 80000000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 7fc00003, 00000000]
  #_ REGISTER_IN v5 [7fc00004, ff800002, 3f800000, 00000000]
  vnmsubfp v6, v3, v4, v5
  blr
  #_ REGISTER_OUT v6 [7fc00001, ffc00002, 7fc00003, 00000000]
  #_ REGISTER_OUT v3 [7fc00001, 3f800000, 3f800000, 80000000]
  #_ REGISTER_OUT v4 [3f800000, 3f800000, 7fc00003, 00000000]
  #_ REGISTER_OUT v5 [7fc00004, ff800002, 3f800000, 00000000]
