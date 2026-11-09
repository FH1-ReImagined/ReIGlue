#version 460
#extension GL_EXT_samplerless_texture_functions : require

// FH1 (#640): which of the 8 stencil bits are set anywhere in rows [y_begin, y_end) of a depth
// render target's stencil, for render target ownership transfers on devices without
// VK_EXT_shader_stencil_export: those copy the stencil in one draw per bit, and with this mask
// (conditional rendering) only the draws of bits that are set run.
// Word `slot * 8 + bit` of the buffer, zeroed before, becomes 1 if the bit is set in any sample.
// Built twice: with MULTISAMPLE defined for sources with more than one sample.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform StencilBitMaskConstants {
  uint width;
  uint y_begin;
  uint y_end;
  uint samples;
  uint slot;
} constants;

layout(set = 0, binding = 0) buffer StencilBitMask {
  uint bits[];
} mask;

// Binding 0 of the transfer source set is the depth, unused here.
#ifdef MULTISAMPLE
layout(set = 1, binding = 1) uniform utexture2DMS stencil_texture;
#else
layout(set = 1, binding = 1) uniform utexture2D stencil_texture;
#endif

shared uint group_bits;

void main() {
  if (gl_LocalInvocationIndex == 0u) {
    group_bits = 0u;
  }
  barrier();
  uvec2 pixel = uvec2(gl_GlobalInvocationID.x, gl_GlobalInvocationID.y + constants.y_begin);
  uint bits = 0u;
  if (pixel.x < constants.width && pixel.y < constants.y_end) {
#ifdef MULTISAMPLE
    for (uint i = 0u; i < constants.samples; ++i) {
      bits |= texelFetch(stencil_texture, ivec2(pixel), int(i)).r;
    }
#else
    bits = texelFetch(stencil_texture, ivec2(pixel), 0).r;
#endif
  }
  if (bits != 0u) {
    atomicOr(group_bits, bits & 0xFFu);
  }
  barrier();
  if (gl_LocalInvocationIndex == 0u && group_bits != 0u) {
    for (uint i = 0u; i < 8u; ++i) {
      if ((group_bits & (1u << i)) != 0u) {
        atomicOr(mask.bits[constants.slot * 8u + i], 1u);
      }
    }
  }
}
