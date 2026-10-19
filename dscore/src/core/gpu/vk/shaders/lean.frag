#version 450
// The lean path's one fragment stage: DS shading once per pixel (texture
// sample, colour combine, toon / highlight), the alpha test, and the colour
// out as raw 6-bit channels with the 5-bit alpha encoded so the pipeline's
// blend factors and MAX alpha reproduce the DS blend. Fog is the resolve's.
// Depth, stencil and blending are the pipeline's; nothing here reads the
// framebuffer.
//
// RG DS: the polygon's constants arrive decoded (lean.vert) and the combine
// runs in float, where every DS step is exact (integers below 2^24, scaling
// by powers of two), so the result matches the integer rules bit for bit
// with fewer instructions. This stage's ALU is what bounds the GPU at 2x.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
// Textures live in a 2D atlas (RGBA8 integer texels pre-converted to the
// 6-bit channels and 5-bit alpha; optimal tiling, the texture unit's cache).
layout(binding = 2) uniform usampler2D texel_atlas;
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) in vec3 v_rgb;
layout(location = 1) in vec2 v_st;
layout(location = 2) flat in ivec4 v_ts;
layout(location = 3) flat in ivec4 v_tt;
layout(location = 4) flat in ivec4 v_tm;
layout(location = 5) flat in vec4 v_stb;
layout(location = 6) flat in vec2 v_af;
layout(location = 0) out vec4 o_col;
layout(location = 1) out float o_fog;   // the polygon's fog bit; blended with MIN so a pixel keeps fog only if every layer had it (the DS rule)
#if defined(DS_EARLY) || defined(DS_NODISCARD)
// Polygons whose alpha cannot fail the test (no cut-out texture): no
// discard, so the depth test runs before shading and overdraw costs nothing.
// DS_NODISCARD (RG DS, measured): translucent pass B with its discards
// replaced by a blend no-op, so early ZS runs here too; the stencil and depth
// updates then happen for those fragments as well (vk_lean.cpp chooses when).
layout(early_fragment_tests) in;
#endif
// RG DS (measured, DS_LEAN_HALFPX): the viewport sits half a pixel on, so
// plain centre interpolation already reads the DS corner sample.
layout(constant_id = 0) const bool DS_HALFPX = false;

// A toon table entry (RGB555) as 6-bit channels: bits 1-5 of each field, +1 when non-zero.
vec3 toon6(uint c) {
  uvec3 v = (uvec3(c << 1, c >> 4, c >> 9)) & 0x3Eu;
  return vec3(v + uvec3(notEqual(v, uvec3(0u))));
}

void main() {
  if ((pc.f.flags & DS_FF_DEBUG_FLAT) != 0u) { o_col = vec4(31.0, 31.0, 31.0, 248.0) / 255.0; o_fog = 0.0; return; }
  // The DS evaluates a pixel at its top-left corner; with vertices on pixel
  // corners that is half a pixel before the centre, so attributes are read
  // there. On an edge pixel the corner can lie outside the polygon and the
  // value is an extrapolation, so colours are clamped to their range and
  // texcoords to the polygon's own (v_stb), never a texel past the polygon.
  vec3 rgb = clamp(DS_HALFPX ? v_rgb : interpolateAtOffset(v_rgb, vec2(-0.5)), vec3(0.0), vec3(511.0));
#ifdef DS_CENTROID
  vec2 st = clamp(interpolateAtCentroid(v_st), v_stb.xy, v_stb.zw);
#else
  vec2 st = clamp(DS_HALFPX ? v_st : interpolateAtOffset(v_st, vec2(-0.5)), v_stb.xy, v_stb.zw);
#endif
  vec3 v = floor(round(rgb) * 0.125);   // 9-bit colour to 6 bits (>> 3)
  float vr = v.r;                 // toon and highlight both index by the vertex red
  int mode = v_tm.z;
  float pa = v_af.x;
  bool highlight = (pc.f.dispcnt & 2u) != 0u;
  // Mode 2 = toon or highlight (DISP3DCNT bit 1). Toon replaces the vertex
  // colour from the 32-entry table; highlight greys to its red and ADDS the
  // table entry after the texture combine (below).
  if (mode == 2) v = highlight ? vec3(vr) : toon6(ps.toon[uint(vr) >> 1]);
  vec3 c = v;
  float a = pa;
  if (v_tm.w != 0) {
    // Texcoords truncate as the DS 12.4 >> 4 does; +0.01 (in 12.4 units) absorbs float error.
    ivec2 s = ivec2(floor(st + 0.01 / 16.0));
    // Clamp (bounds 0..size-1), or repeat (bounds open, size-1 mask), mirrored
    // on odd repeats where flipping: (size-1) - x is x ^ (size-1) for powers of two.
    ivec2 wm = ivec2(v_ts.w, v_tt.w);
    ivec2 cl = clamp(s, ivec2(v_ts.x, v_tt.x), ivec2(v_ts.y, v_tt.y));
    ivec2 tc = (cl & wm) ^ (wm & -ivec2(notEqual(cl & ivec2(v_ts.z, v_tt.z), ivec2(0))));
    vec4 t = vec4(texelFetch(texel_atlas, v_tm.xy + tc, 0));
    if ((mode & 1) != 0) {   // decal (and shadow)
      c = t.a == 0.0 ? v : t.a == 31.0 ? t.rgb : floor((t.rgb * t.a + v * (31.0 - t.a)) * (1.0 / 32.0));
    } else {                 // modulate (and toon / highlight)
      c = floor(((t.rgb + 1.0) * (v + 1.0) - 1.0) * (1.0 / 64.0));
      a = floor(((t.a + 1.0) * (pa + 1.0) - 1.0) * (1.0 / 32.0));
    }
  }
  if (mode == 2 && highlight) c = min(c + toon6(ps.toon[uint(vr) >> 1]), vec3(63.0));
#ifndef DS_EARLY
#ifdef DS_NODISCARD
  // Pass B without discard: a fragment the alpha test or the alpha-31 rule
  // would drop leaves the colour as it is (alpha 0 blends nothing in, the
  // MAX alpha keeps the destination's, and fog 1 keeps its flag under MIN).
  if (a <= float(pc.f.alpha_ref) || a == 31.0) { o_col = vec4(0.0); o_fog = 1.0; return; }
#else
  if (a <= float(pc.f.alpha_ref)) discard;
#endif
#endif
  // A translucent polygon's pixels are opaque where their final alpha is 31
  // (written with depth, no translucent flag, so a later polygon of the
  // same id draws over them) and translucent elsewhere. The run is drawn
  // twice: DS_ALPHA_SEL 1 keeps the opaque pixels, 2 the translucent ones.
#if DS_ALPHA_SEL == 1
  if (a != 31.0) discard;
#elif DS_ALPHA_SEL == 2 && !defined(DS_NODISCARD)
  if (a == 31.0) discard;
#endif
  // Alpha byte: 0 stays 0 (undrawn), else (a + 1) * 8: the blend's SRC_ALPHA is (a + 1) / 32 to within 1/256.
  float ab = a == 0.0 ? 0.0 : min(255.0, (a + 1.0) * 8.0);
  o_col = vec4(c, ab) / 255.0;
  o_fog = v_af.y;
}
