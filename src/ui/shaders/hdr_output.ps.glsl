#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_hdr: the SDR stand-in (guest output and UI, sampled through its
// sRGB view, so linear) to the scRGB swapchain, where 1.0 is 80 nits. The
// stand-in's alpha is how much of the pixel is UI: the HUD of the guest output
// and the host UI stay at the paper white, and only the scene's highlights
// are expanded towards the highlight peak.

layout(push_constant) uniform HdrOutputConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) float paper_white_scale;  // Paper white nits / 80.
  float highlight_scale;  // Highlight peak / paper white - 1, 0 for none.
  float knee;             // Brightness where the expansion starts.
  float knee_range_inv;   // 1 / (1 - knee).
  // 0 decodes the stand-in as piecewise sRGB, otherwise as this power, as an
  // SDR display with that gamma would show it.
  float gamma;
  // The brightness the expansion follows: 0 is the luminance, 1 the largest
  // channel, so that saturated lights (red brake lights) expand too.
  float max_channel_weight;
  // Pixels left of it show the SDR image as Windows composes SDR content
  // (piecewise sRGB at the paper white), with a divider; negative for none.
  float compare_x;
  // A calibration pattern under the UI (kPattern*), and its value.
  uint pattern;
  float pattern_value;  // kPatternPeak: the tested peak, nits / 80.
  float pad0;
  vec2 size;  // The swapchain's size, for the patterns.
} constants;

const uint kPatternNone = 0u;
const uint kPatternBlack = 1u;
const uint kPatternPaperWhite = 2u;
const uint kPatternPeak = 3u;

layout(set = 0, binding = 0) uniform texture2D stand_in;

layout(location = 0) out vec4 output_color;

float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }

vec3 SrgbEncode(vec3 linear) {
  linear = clamp(linear, 0.0, 1.0);
  return mix(1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, linear * 12.92,
             lessThanEqual(linear, vec3(0.0031308)));
}

// An SDR code value as the image's decode shows it, in scRGB.
float SdrLevel(float code) {
  float linear = constants.gamma > 0.0
      ? pow(code, constants.gamma)
      : (code <= 0.04045 ? code / 12.92 : pow((code + 0.055) / 1.055, 2.4));
  return linear * constants.paper_white_scale;
}

bool InRect(vec2 uv, vec2 center, vec2 half_size) {
  return all(lessThan(abs(uv - center), half_size));
}

// Calibration occupies a fixed strip, independent of guest HUD coverage.
// The rest of the game remains visible for judging the surrounding picture.
const float kPatternTop = 0.62;
const float kPatternBottom = 0.88;

vec3 Pattern(vec2 pixel) {
  vec2 uv = pixel / constants.size;
  if (uv.y < kPatternTop || uv.y > kPatternBottom) {
    return vec3(0.0);
  }
  uv.y = (uv.y - kPatternTop) / (kPatternBottom - kPatternTop);
  // Squares in the band's own coordinates.
  vec2 square =
      vec2(constants.size.y * (kPatternBottom - kPatternTop) / constants.size.x, 1.0);
  if (constants.pattern == kPatternBlack) {
    // Black with squares at SDR code values 1 % to 6 %: the TV's brightness
    // is right when the 1 % square is invisible and the 2 % one just shows.
    for (int i = 0; i < 6; ++i) {
      if (InRect(uv, vec2((float(i) + 0.5) / 6.0, 0.5), square * 0.3)) {
        return vec3(SdrLevel(float(i + 1) / 100.0));
      }
    }
    return vec3(0.0);
  }
  if (constants.pattern == kPatternPaperWhite) {
    // A white square beside SDR gray steps on mid gray: white should look
    // like a white sheet of paper in the room, the steps as distinct as on
    // an SDR screen.
    if (InRect(uv, vec2(0.2, 0.5), square * 0.35)) {
      return vec3(SdrLevel(1.0));
    }
    if (uv.y > 0.3 && uv.y < 0.7 && uv.x > 0.4 && uv.x < 0.95) {
      return vec3(SdrLevel(floor((uv.x - 0.4) / 0.55 * 11.0) / 10.0));
    }
    return vec3(SdrLevel(0.5));
  }
  if (constants.pattern == kPatternPeak) {
    // Four measured levels up to the configured display peak. Do not send
    // an unrelated 10,000-nit reference above the user's selected peak.
    return vec3(constants.pattern_value * (floor(min(uv.x, 0.9999) * 4.0) + 1.0) / 4.0);
  }
  return vec3(0.0);
}

void main() {
  vec4 sdr = texelFetch(stand_in, ivec2(gl_FragCoord.xy), 0);
  float ui = clamp(sdr.a, 0.0, 1.0);
  vec3 linear = sdr.rgb;
  float y = gl_FragCoord.y / constants.size.y;
  if (constants.pattern != kPatternNone && y >= kPatternTop && y <= kPatternBottom) {
    output_color = vec4(Pattern(gl_FragCoord.xy), 1.0);
    return;
  }
  if (constants.compare_x >= 0.0 && gl_FragCoord.x < constants.compare_x) {
    // The SDR reference half.
    output_color = vec4(linear * constants.paper_white_scale, 1.0);
    if (gl_FragCoord.x > constants.compare_x - 3.0) {
      output_color.rgb = vec3(constants.paper_white_scale);
    }
    return;
  }
  if (constants.gamma > 0.0) {
    linear = pow(SrgbEncode(linear), vec3(constants.gamma));
  }
  float luminance = dot(linear, vec3(0.2126, 0.7152, 0.0722));
  float brightness = mix(luminance, max3(linear), constants.max_channel_weight);
  float t = clamp((brightness - constants.knee) * constants.knee_range_inv, 0.0, 1.0);
  // Smooth from the knee, reaching the peak at SDR white. Scaling all three
  // channels together keeps the hue.
  float boost = 1.0 + constants.highlight_scale * t * t * (1.0 - ui);
  vec3 color = linear * (constants.paper_white_scale * boost);

  output_color = vec4(color, 1.0);
}
