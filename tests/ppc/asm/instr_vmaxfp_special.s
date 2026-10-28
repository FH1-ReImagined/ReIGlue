# Cases for the rules of ReIGlue 0400 and 0401 that xenia-canary's recorded cases do not reach:
# invalid operations (the default QNaN 0x7FC00000), infinities, signed zeros, and the NaN order
# and operand roles of the VMX128 forms (the order by role is carried over from vmaddfp's
# recorded cases). Expected values from the rules that match all of xenia-canary's recorded
# vmaddfp, vnmsubfp, vmaxfp and vminfp cases; the ones that need one rounding are in
# ../fma_cases.txt.

test_vmaxfp_special_1:
  # infinities: vmaxfp(-inf, +inf), vmaxfp(-inf, -FLT_MAX), a signaling NaN in vB (quieted),
  # vmaxfp(-0, -inf)
  #_ REGISTER_IN v3 [ff800000, ff800000, 7f800000, 80000000]
  #_ REGISTER_IN v4 [7f800000, ff7fffff, 7f800001, ff800000]
  vmaxfp v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7f800000, ff7fffff, 7fc00001, 80000000]
  #_ REGISTER_OUT v3 [ff800000, ff800000, 7f800000, 80000000]
  #_ REGISTER_OUT v4 [7f800000, ff7fffff, 7f800001, ff800000]
