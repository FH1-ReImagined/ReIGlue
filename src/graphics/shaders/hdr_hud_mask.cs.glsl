#version 460
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// Copyright (c) 2026 ReIGlue contributors. SPDX-License-Identifier: BSD-3-Clause
#include "../../../include/rex/ui/hdr_coverage.h"
layout(local_size_x=16,local_size_y=8,local_size_z=1) in;
layout(push_constant) uniform Dimensions {
  uvec2 extent;
  uint swap_rb;
  float source_quantum;
} grid;
layout(set=0,binding=0) uniform texture2D finished;
layout(set=0,binding=1) uniform texture2D background;
layout(set=0,binding=2,rgb10_a2) uniform image2D destination;

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel,grid.extent))) return;
  vec3 final_color = texelFetch(finished,ivec2(pixel),0).rgb;
  if (grid.swap_rb != 0u) final_color = final_color.bgr;
  vec3 scene = texelFetch(background,ivec2(pixel),0).rgb;
  vec3 lower_bound = vec3(
      HdrOverlayLowerBound(final_color.r,scene.r,grid.source_quantum),
      HdrOverlayLowerBound(final_color.g,scene.g,grid.source_quantum),
      HdrOverlayLowerBound(final_color.b,scene.b,grid.source_quantum));
  vec4 output_value = imageLoad(destination,ivec2(pixel));
  // The target only has two alpha bits; preserve any evidenced overlay instead
  // of rounding a faint but real overlay's fractional coverage back to zero.
  output_value.a = any(greaterThan(lower_bound,vec3(0.0))) ? 1.0 : 0.0;
  imageStore(destination,ivec2(pixel),output_value);
}
