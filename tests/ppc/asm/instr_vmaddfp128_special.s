# Cases for the rules of ReIGlue 0400 and 0401 that xenia-canary's recorded cases do not reach:
# invalid operations (the default QNaN 0x7FC00000), infinities, signed zeros, and the NaN order
# and operand roles of the VMX128 forms (the order by role is carried over from vmaddfp's
# recorded cases). Expected values from the rules that match all of xenia-canary's recorded
# vmaddfp, vnmsubfp, vmaxfp and vminfp cases; the ones that need one rounding are in
# ../fma_cases.txt.

test_vmaddfp128_special_1:
  # NaN order as vmaddfp's, by role: vA, then vD (the addend), then vB;
  # -inf * 0 + 1 is invalid: 7fc00000
  #_ REGISTER_IN v3 [7fc00002, 7f800004, 3f800000, 3f800000]
  #_ REGISTER_IN v4 [7fc00001, 3f800000, 3f800000, ff800000]
  #_ REGISTER_IN v5 [7fc00003, 7fc00005, ff800006, 00000000]
  vmaddfp128 v3, v4, v5, v3
  blr
  #_ REGISTER_OUT v3 [7fc00001, 7fc00004, ffc00006, 7fc00000]
  #_ REGISTER_OUT v4 [7fc00001, 3f800000, 3f800000, ff800000]
  #_ REGISTER_OUT v5 [7fc00003, 7fc00005, ff800006, 00000000]

test_vmaddfp128_special_2:
  # one rounding: 1.1 * 1.1 + 1.1 = 4013d70b (twice rounded: 4013d70a)
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v5 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  vmaddfp128 v3, v4, v5, v3
  blr
  #_ REGISTER_OUT v3 [40000000, 40700000, 4013d70b, 40b051eb]
  #_ REGISTER_OUT v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v5 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
