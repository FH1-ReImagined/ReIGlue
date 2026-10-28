#version 460
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// Copyright (c) 2026 ReIGlue contributors. SPDX-License-Identifier: BSD-3-Clause
#include "../../../include/rex/ui/hdr_color.h"

// Binary push-constant interface: 16-byte rectangle precedes these 48 bytes.
layout(push_constant) uniform Output {
  layout(offset=16) float white;
  float headroom;
  float knee;
  float inverse_span;
  float source_gamma;
  float saturation_weight;
  float split;
  uint calibration;
  float peak;
  float reserved;
  vec2 resolution;
} settings;
layout(set=0,binding=0) uniform texture2D composed;
layout(location=0) out vec4 result;

float DisplayCode(float code) {
  float light = settings.source_gamma > 0.0
      ? pow(clamp(code, 0.0, 1.0), settings.source_gamma) : HdrDecodeSrgb(code);
  return light * settings.white;
}

float Calibration(vec2 position) {
  // The 62%-88% height band leaves the surrounding game visible.
  float row = (position.y - 0.62) / 0.26;
  if (settings.calibration == 3u)
    return settings.peak * float(min(int(position.x * 4.0), 3) + 1) * 0.25;
  if (settings.calibration == 1u) {
    int column = min(int(position.x * 6.0), 5);
    float center = (float(column) + 0.5) / 6.0;
    float half_width = 0.078 * settings.resolution.y / settings.resolution.x;
    bool inside = abs(position.x - center) < half_width && abs(row - 0.5) < 0.3;
    return inside ? DisplayCode(float(column + 1) * 0.01) : 0.0;
  }
  float half_width = 0.091 * settings.resolution.y / settings.resolution.x;
  if (abs(position.x - 0.2) < half_width && abs(row - 0.5) < 0.35)
    return DisplayCode(1.0);
  if (row > 0.3 && row < 0.7 && position.x > 0.4 && position.x < 0.95)
    return DisplayCode(floor((position.x - 0.4) * 20.0) * 0.1);
  return DisplayCode(0.5);
}

void main() {
  vec2 position = gl_FragCoord.xy / settings.resolution;
  vec4 source = texelFetch(composed, ivec2(gl_FragCoord.xy), 0);
  vec3 light;
  if (settings.calibration != 0u && position.y >= 0.62 && position.y <= 0.88) {
    light = vec3(Calibration(position));
  } else if (settings.split >= 0.0 && gl_FragCoord.x < settings.split) {
    light = gl_FragCoord.x > settings.split - 3.0 ? vec3(settings.white)
                                                : source.rgb * settings.white;
  } else {
    light = vec3(HdrSourceLight(source.r, settings.source_gamma),
                 HdrSourceLight(source.g, settings.source_gamma),
                 HdrSourceLight(source.b, settings.source_gamma));
    float energy = dot(light, vec3(0.2126, 0.7152, 0.0722));
    float strongest = max(max(light.r, light.g), light.b);
    float brightness = energy + settings.saturation_weight * (strongest - energy);
    light *= HdrGain(brightness, source.a, settings.white, settings.headroom,
                     settings.knee, settings.inverse_span);
  }
  result = vec4(light, 1.0);
}
