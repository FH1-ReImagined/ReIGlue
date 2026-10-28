#version 460
// Copyright (c) 2026 ReIGlue contributors. SPDX-License-Identifier: BSD-3-Clause
layout(push_constant) uniform CoveragePlacement {
  layout(offset=16) ivec2 origin;
  vec2 reciprocal_extent;
} placement;
layout(set=0,binding=0) uniform texture2D coverage_image;
layout(set=0,binding=1) uniform sampler coverage_filter;
layout(location=0) out vec4 result;
void main() {
  vec2 position = placement.reciprocal_extent * (gl_FragCoord.xy - vec2(placement.origin));
  float coverage = textureLod(sampler2D(coverage_image, coverage_filter), position, 0.0).a;
  result = vec4(0.0, 0.0, 0.0, clamp(coverage, 0.0, 1.0));
}
