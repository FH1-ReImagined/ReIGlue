/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Compute shader to downscale scaled resolve buffer data back to 1x resolution.
// Operates on 32x32 tiled data format used by Xbox 360.
// Each thread handles one output pixel (one 32x32 tile = 1024 threads).
//
// By default, picks the top-left pixel of each scale_x * scale_y block.
// Bit 0 of xe_downscale_half_pixel_offset samples from (scale/2, scale/2)
// within each block to compensate for the half-pixel offset becoming a
// full-pixel offset at higher resolutions.
//
// 1120: the host pixels of a guest pixel are addressed in the scaled
// resolve's 16-byte quads (see main), at every pixel size.

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(push_constant) uniform ResolveDownscaleConstants {
  uint xe_downscale_scale_x;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_scale_y;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_pixel_size_log2;   // 0=8bit, 1=16bit, 2=32bit, 3=64bit
  uint xe_downscale_tile_count;        // Number of 32x32 tiles to process
  uint xe_downscale_source_offset_bytes;   // Byte offset into source buffer
  // Bit 1 enables an RGBA8 box filter (car thumbnails and unsigned 8:8:8:8
  // color copies, 32-bit only). Bit 2 enables the same box filter for
  // 2:10:10:10 pixels, whose endianness (8in16, 8in32 or 16in32, swapped
  // within the pixel) is in bits 4 and 5.
  // Bit 0 applies half-pixel offset correction by sampling from
  // (scale/2, scale/2) within each scaled block instead of (0, 0).
  // This compensates for the D3D9-style half-pixel offset used by Xbox 360
  // games, which at Nx resolution scaling shifts rendered content by
  // (N/2, N/2) host pixels.
  uint xe_downscale_half_pixel_offset;
};

// Source buffer (scaled resolve data)
layout(std430, set = 0, binding = 0) readonly buffer SourceBuffer {
  uint data[];
} xe_resolve_source;

// Destination buffer (1x resolution)
layout(std430, set = 0, binding = 1) writeonly buffer DestBuffer {
  uint data[];
} xe_resolve_dest;

// Groupshared memory for coalescing sub-32-bit writes
shared uint gs_tile_data[32 * 32];

// A 32-bit pixel's endianness within itself (Endian128 8in16, 8in32, 16in32);
// each is its own inverse.
uint XeSwap32(uint value, uint endian) {
  if (((endian ^ (endian >> 1u)) & 1u) != 0u) {
    value = ((value & 0x00FF00FFu) << 8u) | ((value & 0xFF00FF00u) >> 8u);
  }
  if ((endian & 2u) != 0u) {
    value = (value << 16u) | (value >> 16u);
  }
  return value;
}

void main() {
  uint tile_index = gl_WorkGroupID.x;

  // Early out if beyond tile count
  if (tile_index >= xe_downscale_tile_count) {
    return;
  }

  uint row = gl_LocalInvocationID.y;
  uint column = gl_LocalInvocationID.x;
  uint pixel_index = row * 32u + column;  // 0-1023

  uint pixel_size = 1u << xe_downscale_pixel_size_log2;
  uint tile_size_1x = 32u * 32u * pixel_size;
  uint scale_xy = xe_downscale_scale_x * xe_downscale_scale_y;

  // Destination offset in 1x buffer
  uint dst_byte_offset = tile_index * tile_size_1x +
                         pixel_index * pixel_size;
  uint dst_offset = dst_byte_offset >> 2u;

  // 1120: the scaled resolve stores each 16-byte guest quad (16 >> log2
  // pixels) as scale_x * scale_y 16-byte host quads, a column of scale_y of
  // them after the other; host quad q of a guest quad holds pixels
  // (16 >> log2) * (q / scale_y) onwards of row q % scale_y of the guest
  // quad's host pixels. The guest pixel's quad and its place in it:
  uint quad_pixels_log2 = 4u - xe_downscale_pixel_size_log2;
  uint quad_base = (dst_byte_offset >> 4u) * scale_xy;
  uint pixel_in_quad = (dst_byte_offset & 15u) >> xe_downscale_pixel_size_log2;
  // The host pixel to take: (0, 0) of the guest pixel's, or (scale/2,
  // scale/2) with bit 0, to compensate for the D3D9-style half-pixel offset
  // shifting content by (N/2, N/2) pixels at Nx resolution.
  uint sample_x = pixel_in_quad * xe_downscale_scale_x;
  uint sample_y = 0u;
  if ((xe_downscale_half_pixel_offset & 1u) != 0u && scale_xy > 1u) {
    sample_x += xe_downscale_scale_x >> 1u;
    sample_y = xe_downscale_scale_y >> 1u;
  }
  // Add source_offset_bytes to handle buffer-relative addressing; computed in
  // bytes to preserve the sub-dword byte position.
  uint src_byte_offset = xe_downscale_source_offset_bytes +
      (quad_base + (sample_x >> quad_pixels_log2) * xe_downscale_scale_y +
       sample_y) * 16u +
      ((sample_x & ((1u << quad_pixels_log2) - 1u)) <<
       xe_downscale_pixel_size_log2);
  uint src_offset = src_byte_offset >> 2u;

  // Copy pixel based on size
  switch (xe_downscale_pixel_size_log2) {
    case 0u: {  // 8-bit - use groupshared to coalesce 4 bytes into 32-bit writes
      // Load the byte value
      uint src_word = xe_resolve_source.data[src_offset];
      uint byte_val = (src_word >> ((src_byte_offset & 3u) * 8u)) & 0xFFu;

      // Each group of 4 threads packs into one uint
      uint pack_index = pixel_index >> 2u;   // Which uint (0-255)
      uint byte_pos = pixel_index & 3u;      // Which byte in uint (0-3)

      // Pack byte into shared memory
      uint contribution = byte_val << (byte_pos * 8u);
      if (byte_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      barrier();
      if (byte_pos != 0u) {
        atomicOr(gs_tile_data[pack_index], contribution);
      }
      barrier();

      // First thread of each 4 writes the packed uint
      if (byte_pos == 0u) {
        xe_resolve_dest.data[tile_index * (32u * 32u / 4u) + pack_index] =
            gs_tile_data[pack_index];
      }
      break;
    }
    case 1u: {  // 16-bit - use groupshared to coalesce 2 shorts into 32-bit writes
      // Load the short value
      uint src_word = xe_resolve_source.data[src_offset];
      uint short_val = (src_word >> ((src_byte_offset & 2u) * 8u)) & 0xFFFFu;

      // Each group of 2 threads packs into one uint
      uint pack_index = pixel_index >> 1u;   // Which uint (0-511)
      uint short_pos = pixel_index & 1u;     // Which short in uint (0-1)

      // Pack short into shared memory
      uint contribution = short_val << (short_pos * 16u);
      if (short_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      barrier();
      if (short_pos != 0u) {
        atomicOr(gs_tile_data[pack_index], contribution);
      }
      barrier();

      // First thread of each 2 writes the packed uint
      if (short_pos == 0u) {
        xe_resolve_dest.data[tile_index * (32u * 32u * 2u / 4u) + pack_index] =
            gs_tile_data[pack_index];
      }
      break;
    }
    case 2u: {  // 32-bit
      if ((xe_downscale_half_pixel_offset & 6u) != 0u) {
        // Opt-in box filter (car thumbnails, unsigned 8:8:8:8 and 2:10:10:10
        // color copies): the average of the guest pixel's scale_x * scale_y
        // host pixels. Other 32-bit formats and depth keep point sampling.
        bool rgb10 = (xe_downscale_half_pixel_offset & 4u) != 0u;
        uint endian = (xe_downscale_half_pixel_offset >> 4u) & 3u;
        uvec4 sum = uvec4(0u);
        for (uint sample_index = 0u; sample_index < scale_xy; ++sample_index) {
          uint x = pixel_in_quad * xe_downscale_scale_x +
              sample_index % xe_downscale_scale_x;
          uint y = sample_index / xe_downscale_scale_x;
          uint source_quad = quad_base + (x >> 2u) * xe_downscale_scale_y + y;
          uint source_word = (xe_downscale_source_offset_bytes >> 2u) +
              source_quad * 4u + (x & 3u);
          uint value = xe_resolve_source.data[source_word];
          if (rgb10) {
            value = XeSwap32(value, endian);
            sum += uvec4(value & 1023u, (value >> 10u) & 1023u,
                         (value >> 20u) & 1023u, value >> 30u);
          } else {
            sum += uvec4(value & 255u, (value >> 8u) & 255u,
                         (value >> 16u) & 255u, value >> 24u);
          }
        }
        uvec4 average = (sum + uvec4(scale_xy / 2u)) / scale_xy;
        xe_resolve_dest.data[dst_offset] = rgb10
            ? XeSwap32(average.x | (average.y << 10u) | (average.z << 20u) |
                       (average.w << 30u), endian)
            : average.x | (average.y << 8u) | (average.z << 16u) |
                  (average.w << 24u);
      } else {
        xe_resolve_dest.data[dst_offset] = xe_resolve_source.data[src_offset];
      }
      break;
    }
    case 3u: {  // 64-bit - direct copy (2 uints)
      xe_resolve_dest.data[dst_offset] = xe_resolve_source.data[src_offset];
      xe_resolve_dest.data[dst_offset + 1u] =
          xe_resolve_source.data[src_offset + 1u];
      break;
    }
  }
}
