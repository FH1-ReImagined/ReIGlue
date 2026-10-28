# Independently calculated destination checks, separate from the imported cases.
# vmaddcfp128 vD, vA, vD, vB computes vA * old(vD) + vB, rounded once.

test_vmaddcfp128_destination_operands:
  # (3*2+5, 4*(-3)+6, (-8)*0.5+7, 16*(-0.25)-8) = (11, -6, 3, -12).
  # Distinct lanes and nonzero old vD distinguish the multiply input from the addend.
  #_ REGISTER_IN v3 [40000000, c0400000, 3f000000, be800000]
  #_ REGISTER_IN v4 [40400000, 40800000, c1000000, 41800000]
  #_ REGISTER_IN v5 [40a00000, 40c00000, 40e00000, c1000000]
  vmaddcfp128 v3, v4, v3, v5
  blr
  #_ REGISTER_OUT v3 [41300000, c0c00000, 40400000, c1400000]
  #_ REGISTER_OUT v4 [40400000, 40800000, c1000000, 41800000]
  #_ REGISTER_OUT v5 [40a00000, 40c00000, 40e00000, c1000000]

test_vmaddcfp128_destination_fused:
  # (1+2^-23)*(1-2^-23)-1 = -2^-46 exactly. Rounding the product first gives zero.
  #_ REGISTER_IN v3 [3f7ffffe, 3f7ffffe, 3f7ffffe, 3f7ffffe]
  #_ REGISTER_IN v4 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_IN v5 [bf800000, bf800000, bf800000, bf800000]
  vmaddcfp128 v3, v4, v3, v5
  blr
  #_ REGISTER_OUT v3 [a8800000, a8800000, a8800000, a8800000]
  #_ REGISTER_OUT v4 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_OUT v5 [bf800000, bf800000, bf800000, bf800000]

test_vmaddcfp128_destination_alias:
  # Both multiply inputs alias vD: (2^2-1, (-3)^2+1, 0.5^2+1, (-0.25)^2+1).
  #_ REGISTER_IN v3 [40000000, c0400000, 3f000000, be800000]
  #_ REGISTER_IN v5 [bf800000, 3f800000, 3f800000, 3f800000]
  vmaddcfp128 v3, v3, v3, v5
  blr
  #_ REGISTER_OUT v3 [40400000, 41200000, 3fa00000, 3f880000]
  #_ REGISTER_OUT v5 [bf800000, 3f800000, 3f800000, 3f800000]
