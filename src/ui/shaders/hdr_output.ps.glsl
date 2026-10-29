#version 460
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// Copyright (c) 2026 ReIGlue contributors. SPDX-License-Identifier: BSD-3-Clause
#include "../../../include/rex/ui/hdr_color.h"
#include "../../../include/rex/ui/hdr_calibration.h"

layout(push_constant) uniform Output {
  layout(offset=16) float white;
  float scene_ceiling;
  float diffuse_anchor;
  float curve_exponent;
  float source_gamma;
  float curve_denominator;
  float split;
  uint calibration;
  float peak;
  float reserved;
  vec2 resolution;
} settings;
layout(set=0,binding=0) uniform texture2D composed;
layout(location=0) out vec4 result;

void main() {
  vec4 source = texelFetch(composed, ivec2(gl_FragCoord.xy), 0);
  float calibration = HdrCalibrationSignal(int(settings.calibration),
      gl_FragCoord.x, gl_FragCoord.y, settings.resolution.x, settings.resolution.y,
      settings.white, settings.peak);
  vec3 light;
  if (calibration >= 0.0) {
    light = vec3(calibration);
  } else if (settings.split >= 0.0 && gl_FragCoord.x < settings.split) {
    light = source.rgb * settings.white;
  } else {
    light = vec3(HdrSourceLight(source.r, settings.source_gamma),
                 HdrSourceLight(source.g, settings.source_gamma),
                 HdrSourceLight(source.b, settings.source_gamma));
    float strongest = max(max(light.r, light.g), light.b);
    light *= HdrSceneGain(strongest, source.a, settings.white, settings.scene_ceiling,
        settings.diffuse_anchor, settings.curve_exponent, settings.curve_denominator);
  }
  result = vec4(light, 1.0);
}
