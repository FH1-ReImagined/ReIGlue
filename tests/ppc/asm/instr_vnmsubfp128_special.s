# Cases for the rules of ReIGlue 0400 and 0401 that xenia-canary's recorded cases do not reach:
# invalid operations (the default QNaN 0x7FC00000), infinities, signed zeros, and the NaN order
# and operand roles of the VMX128 forms (the order by role is carried over from vmaddfp's
# recorded cases). Expected values from the rules that match all of xenia-canary's recorded
# vmaddfp, vnmsubfp, vmaxfp and vminfp cases; the ones that need one rounding are in
# ../fma_cases.txt.

test_vnmsubfp128_special_1:
  # NaN order by role: vA, then vD (the addend), then vB, signs kept;
  # -(inf * 1 - inf) is invalid: 7fc00000
  #_ REGISTER_IN v3 [ffc00002, ff800004, 3f800000, 7f800000]
  #_ REGISTER_IN v4 [7fc00001, 3f800000, 3f800000, 7f800000]
  #_ REGISTER_IN v5 [7fc00003, 3f800000, 7fc00005, 3f800000]
  vnmsubfp128 v3, v4, v5, v3
  blr
  #_ REGISTER_OUT v3 [7fc00001, ffc00004, 7fc00005, 7fc00000]
  #_ REGISTER_OUT v4 [7fc00001, 3f800000, 3f800000, 7f800000]
  #_ REGISTER_OUT v5 [7fc00003, 3f800000, 7fc00005, 3f800000]

test_vnmsubfp128_special_2:
  # one rounding: -(1.1 * 1.1 - 1.1)
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v5 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  vnmsubfp128 v3, v4, v5, v3
  blr
  #_ REGISTER_OUT v3 [80000000, bf400000, bde147b2, bfdae147]
  #_ REGISTER_OUT v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v5 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
