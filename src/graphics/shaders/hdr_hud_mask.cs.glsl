#version 460
#extension GL_EXT_samplerless_texture_functions : require
// Copyright (c) 2026 ReIGlue contributors. SPDX-License-Identifier: BSD-3-Clause
layout(local_size_x=16,local_size_y=8,local_size_z=1) in;
layout(push_constant) uniform Dimensions { uvec2 extent; } grid;
layout(set=0,binding=0) uniform texture2D finished;
layout(set=0,binding=1) uniform texture2D background;
layout(set=0,binding=2,rgb10_a2) uniform image2D destination;

// Each workgroup loads an 18x10 tile, including the one-pixel halo. Dilation
// then reads shared memory; it does not fetch both images nine times per pixel.
shared uint changed[180];
float Distance(vec3 a, vec3 b) {
  vec3 delta = abs(a - b);
  return max(max(delta.x, delta.y), delta.z);
}
float UnorderedDistance(vec3 a, vec3 b) {
  // Different source views can permute color channels. Find the least change
  // among the six channel correspondences; no color-order assumption is needed.
  return min(min(min(Distance(a,b), Distance(a,b.rbg)),
                 min(Distance(a,b.grb), Distance(a,b.gbr))),
             min(Distance(a,b.brg), Distance(a,b.bgr)));
}
void main() {
  ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * ivec2(16,8) - ivec2(1);
  for (uint i=gl_LocalInvocationIndex; i<180u; i+=128u) {
    ivec2 sample_at = clamp(tile_origin + ivec2(i%18u,i/18u), ivec2(0), ivec2(grid.extent)-1);
    float delta = UnorderedDistance(texelFetch(finished,sample_at,0).rgb,
                                    texelFetch(background,sample_at,0).rgb);
    changed[i] = delta > 0.006 ? 1u : 0u;
  }
  barrier();
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel),grid.extent))) return;
  uint first = gl_LocalInvocationID.x + 18u * gl_LocalInvocationID.y;
  uint coverage = 0u;
  for (uint row=0u; row<3u; ++row)
    for (uint column=0u; column<3u; ++column)
      coverage |= changed[first + row*18u + column];
  vec4 output_value = imageLoad(destination,pixel);
  output_value.a = float(coverage);
  imageStore(destination,pixel,output_value);
}
