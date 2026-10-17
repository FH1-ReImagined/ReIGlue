#pragma once
#include <cmath>
#include <cstdint>
namespace rex::graphics {
// FH1 screen-space UI shaders: affine UI primitives use c15, font glyphs c3.
// Other shader families (including projected world labels) keep their layout.
inline int Fh1HudTranslationRegister(uint64_t hash) {
  switch (hash) {
    case 0x79034645B1CB882Bull:
    case 0x984DBF6AF14DBEBDull: return 15;
    case 0xED90DA6EFF5C6BCAull: return 3;
    default: return -1;
  }
}
inline float Fh1HudEdgeAnchor(float x, float w) {
  if (!std::isfinite(x) || !std::isfinite(w) || std::abs(w - 1.0f) > 0.001f) return 0;
  // Preserve the central band for countdowns/centered elements. Translate each
  // draw as a unit rather than stretching vertices or individual glyph shapes.
  return x < -0.25f ? -1.0f : x > 0.25f ? 1.0f : 0.0f;
}
}
