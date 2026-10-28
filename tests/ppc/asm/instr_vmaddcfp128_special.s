# Cases for the rules of ReIGlue 0400 and 0401 that xenia-canary's recorded cases do not reach:
# invalid operations (the default QNaN 0x7FC00000), infinities, signed zeros, and the NaN order
# and operand roles of the VMX128 forms (the order by role is carried over from vmaddfp's
# recorded cases). Expected values from the rules that match all of xenia-canary's recorded
# vmaddfp, vnmsubfp, vmaxfp and vminfp cases; the ones that need one rounding are in
# ../fma_cases.txt.

test_vmaddcfp128_special_1:
  # vD = vA * vD + vB: (3 * 2) + 1, (5 * 3) - 2, (2 * -4) + 0.25, (8 * 0.5) + 100
  # (vA * vB + vD would give 5, -7, -3.5, 800.5)
  #_ REGISTER_IN v3 [40000000, 40400000, c0800000, 3f000000]
  #_ REGISTER_IN v4 [40400000, 40a00000, 40000000, 41000000]
  #_ REGISTER_IN v5 [3f800000, c0000000, 3e800000, 42c80000]
  vmaddcfp128 v3, v4, v3, v5
  blr
  #_ REGISTER_OUT v3 [40e00000, 41500000, c0f80000, 42d00000]
  #_ REGISTER_OUT v4 [40400000, 40a00000, 40000000, 41000000]
  #_ REGISTER_OUT v5 [3f800000, c0000000, 3e800000, 42c80000]

test_vmaddcfp128_special_2:
  # one rounding: 1.1 * 1.1 + 1.1 = 4013d70b
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v5 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  vmaddcfp128 v3, v4, v3, v5
  blr
  #_ REGISTER_OUT v3 [40000000, 40700000, 4013d70b, 40b051eb]
  #_ REGISTER_OUT v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v5 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]

test_vmaddcfp128_special_3:
  # NaN order by role: vA, then vB (the addend), then vD; inf * 0 + 1 is invalid: 7fc00000
  #_ REGISTER_IN v3 [7fc00003, 7fc00006, ff800007, 00000000]
  #_ REGISTER_IN v4 [7fc00001, 3f800000, 3f800000, 7f800000]
  #_ REGISTER_IN v5 [7fc00002, 7f800005, 3f800000, 3f800000]
  vmaddcfp128 v3, v4, v3, v5
  blr
  #_ REGISTER_OUT v3 [7fc00001, 7fc00005, ffc00007, 7fc00000]
  #_ REGISTER_OUT v4 [7fc00001, 3f800000, 3f800000, 7f800000]
  #_ REGISTER_OUT v5 [7fc00002, 7f800005, 3f800000, 3f800000]
