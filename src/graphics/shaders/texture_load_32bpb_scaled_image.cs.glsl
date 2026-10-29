// Direct scaled 32-bit tiled texture upload. See the BSD license in LICENSE.
// Depth conversions adapted from Xenia pixel_formats.xesli:
// Copyright 2022 Ben Vanik. All rights reserved. BSD license (LICENSE).
#version 450
layout(local_size_x = 32, local_size_y = 8) in;
layout(set = 0, binding = 0, r32ui) uniform writeonly uimage2D dest;
layout(set = 1, binding = 0, std430) readonly buffer Source {
  uvec4 data[];
}
source;
layout(push_constant, std430) uniform Constants {
  uint flags, guest_offset, pitch, z_stride;
  uint width, height, depth, host_offset, host_pitch, height_texels;
}
c;
uint tiled(uvec2 p) {
  uint macro = ((p.x >> 5) + (p.y >> 5) * (c.pitch >> 5)) << 9;
  uint micro = ((p.x & 7) + ((p.y & 14) << 2)) << 2;
  uint offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((p.y & 1) << 4);
  return ((offset & ~511u) << 3) + ((p.y & 16) << 7) + ((offset & 448) << 2) +
         (((((p.y & 8) >> 2) + (p.x >> 3)) & 3) << 6) + (offset & 63);
}
// Match the reference depth loaders exactly. In particular UNorm24 is not a
// multiply by 1/16777215: its rounding matters for depth reinterpreted as color.
uint float20e4To32(uint value) {
  if (value == 0u)
    return 0u;
  uint mantissa = value & 0xfffffu;
  uint exponent = value >> 20u;
  if (exponent == 0u) {
    uint shift = 20u - uint(findMSB(mantissa));
    exponent = 1u - shift;
    mantissa = (mantissa << shift) & 0xfffffu;
  }
  return ((exponent + 112u) << 23u) | (mantissa << 3u);
}
void main() {
  uvec2 p = uvec2(gl_GlobalInvocationID.x * 4, gl_GlobalInvocationID.y);
  if (p.x >= c.width || p.y >= c.height)
    return;
  uvec2 scale = uvec2((c.flags >> 4) & 7, (c.flags >> 7) & 7);
  uvec2 quad = uvec2(p.x >> 2, p.y) / scale;
  uint address = c.guest_offset + tiled(uvec2(quad.x << 2, quad.y)) * scale.x * scale.y +
                 (((p.x >> 2) % scale.x) * scale.y + (p.y % scale.y)) * 16;
  uvec4 words = source.data[address >> 4];
  uint endian = (c.flags >> 2) & 3;
  if (((endian ^ (endian >> 1)) & 1) != 0)
    words = ((words & 0x00ff00ffu) << 8) | ((words & 0xff00ff00u) >> 8);
  if ((endian & 2) != 0)
    words = (words << 16) | (words >> 16);
  // Bits 10-11 are reserved by this image loader; the buffer loaders receive
  // the original flags. Zero keeps the existing bit-preserving color path.
  uint depthMode = (c.flags >> 10u) & 3u;
  if (depthMode == 1u) {
    uvec4 depth = words >> 8u;
    words = floatBitsToUint(vec4(depth + (depth >> 23u)) * (1.0 / 16777216.0));
  } else if (depthMode == 2u) {
    for (uint i = 0u; i < 4u; ++i)
      words[i] = float20e4To32(words[i] >> 8u);
  }
  for (uint i = 0; i < 4; ++i)
    if (p.x + i < c.width)
      imageStore(dest, ivec2(p + uvec2(i, 0)), uvec4(words[i]));
}
