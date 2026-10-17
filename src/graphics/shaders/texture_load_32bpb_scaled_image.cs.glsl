// Direct scaled 32-bit tiled texture upload. See the BSD license in LICENSE.
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
  for (uint i = 0; i < 4; ++i)
    if (p.x + i < c.width)
      imageStore(dest, ivec2(p + uvec2(i, 0)), uvec4(words[i]));
}
