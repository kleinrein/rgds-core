#pragma once
// HD 2D on the GPU: the DS picture reconstructed at the output's own resolution, under any screen filter.
//
// Pass 1 (rules, one fragment per native pixel, 256x192) decides everything per native pixel, so pass 2 does almost
// nothing where nothing happens. It reads the pixel and its 8 neighbours first: each corner whose pixel differs from
// both neighbours it faces, one of them by 48 or more (xBR's own "equal" threshold: a structural edge, not the shading
// inside detailed art), is a candidate, and the neighbours within 16 of the pixel are where it may smooth. Only
// candidates read the outer 12 pixels and run Hyllian's xBR level 2 corner tests. HD smooth (hd2d_strength) drops the
// floor to 0 and smooths below 64, so shading and texture steps turn into slopes as well, with wider anti-aliasing. Output, one RGBA8 texel:
// R = state0 * 8 + state1, G = state2 * 8 + state3 (0 none, 1 45 degrees, 2 + shallow, 3 + steep, 4 both),
// B = the corners' neighbour choices (bit k: first axis) + 16 where 2D owns the pixel,
// A = the neighbours it smooths toward (right 1, down 2, left 4, up 8, then down-right, down-left, up-right, up-left).
//
// Pass 2 (reconstruct, one fragment per output pixel): with no corner and no smoothing, the pixel itself (two reads).
// Otherwise a bilinear blend toward the flagged neighbours only (gradients turn smooth, detail stays sharp), then each
// corner edge as a half-plane whose coverage is measured analytically over the fragment's footprint: anti-aliasing
// exactly one output pixel wide at any output size, so the same passes are as sharp at 640x480 as they would be at
// 4K. Blends are in linear light (gamma 2). Colour arithmetic is mediump (fp16 on Mali), coordinates highp.
//
// 3D at 2x-5x: the source is DSCore's 640x480 compose, 2D on its native grid, with the lowest bit of blue 1 where 2D
// alone decides a native pixel and 0 where 3D shows through (invisible to anything else that reads the frame; alpha
// would not be). Native colours are read at block centres; where 3D owns a pixel the fragment keeps the GPU's
// high-resolution 3D, and 2D edges against it use that 3D colour, so 2D outlines over 3D are rebuilt too.

namespace rgds {

// Shared by both passes: the source (one screen's rectangle in an SDL texture, maybe BGRA) read per native pixel.
inline constexpr const char* hd2d_common = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D u_hd_src;
uniform vec4 u_hd_rect;  // this screen in the texture, uv: offset, size
uniform vec2 u_hd_px;    // the screen's size in texels (256x192 native or the 640x480 compose)
uniform float u_hd_swap;     // 1: the texture holds BGRA
uniform float u_hd_mask;     // 1: blue's lowest bit marks 2D ownership (the 3D compose)
// The strength (hd2d_strength): HD smooth rebuilds every edge xBR finds and blends far wider and softer.
uniform mediump float u_hd_edge;   // a corner's neighbours must both differ by this much (structural edges)
uniform mediump float u_hd_smooth; // a pixel blends toward neighbours closer than this
uniform mediump float u_hd_soft;   // anti-aliasing width, in output pixels
const vec2 HD_NATIVE = vec2(256.0, 192.0);
mediump vec4 hd_source(vec2 uv) {
  mediump vec4 c = texture2D(u_hd_src, u_hd_rect.xy + uv * u_hd_rect.zw);
  return mix(c, c.bgra, u_hd_swap);
}
// Native pixel p (integral), clamped to the screen; alpha 1 where 2D owns it.
mediump vec4 hd_fetch(vec2 p) {
  p = clamp(p, vec2(0.0), HD_NATIVE - 1.0);
  mediump vec4 c = hd_source((floor((p + 0.5) * (u_hd_px / HD_NATIVE)) + 0.5) / u_hd_px);
  c.a = mix(1.0, mod(floor(c.b * 255.0 + 0.5), 2.0), u_hd_mask);
  return c;
}
// The perceptual distance of the CPU reference, in 0..255 units: luma plus a quarter of both chroma differences.
mediump float hd_df(mediump vec3 a, mediump vec3 b) {
  mediump vec3 d = floor((a - b) * 255.0 + 0.5); // whole levels, so thresholds decide exactly
  mediump float y = dot(d, vec3(27.0, 92.0, 9.0) / 128.0);
  return abs(y) + 0.25 * (abs(d.b - y) + abs(d.r - y));
}
bool hd_differ(mediump vec3 a, mediump vec3 b) { return any(notEqual(a, b)); }
)";

// u_hd_edge, u_hd_smooth, u_hd_soft for HD and for HD smooth; dscore's rgds_hd2d_rules.hpp uses the first two.
inline constexpr float hd2d_strength[2][3] = {{48.f, 16.f, 1.f}, {0.f, 64.f, 1.5f}};

inline constexpr const char* hd2d_vertex = R"(
attribute vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
)";

inline constexpr const char* hd2d_rules = R"(
const mediump float HD_EQ = 48.0;     // xBR's "same colour" distance
// Corner tests past level 0. Arguments rotated per corner; fb, hd, hf are the side diagonals' distances, ei, eg, ec
// the pixel's to its diagonals, ef, eh to the two neighbours it faces.
mediump float hd_corner(mediump vec3 e, mediump vec3 b, mediump vec3 c, mediump vec3 d, mediump vec3 f, mediump vec3 g,
                     mediump vec3 h, mediump vec3 i, mediump vec3 i4, mediump vec3 i5, mediump vec3 h5,
                     mediump vec3 f4, mediump float fb, mediump float hd, mediump float hf, mediump float ei,
                     mediump float eg, mediump float ec, mediump float ef, mediump float eh, out mediump float take) {
  take = ef <= eh ? 1.0 : 0.0;
  mediump float fi4 = hd_df(f, i4), hi5 = hd_df(h, i5);
  // Level 1: no rule on a real corner of the art.
  if (!((fb >= HD_EQ && hd >= HD_EQ) || (ei < HD_EQ && fi4 >= HD_EQ && hi5 >= HD_EQ) || eg < HD_EQ || ec < HD_EQ)) return 0.0;
  // The edge runs along f-h when the e-i direction is the busier one.
  if (ec + eg + hd_df(i, h5) + hd_df(i, f4) + 4.0 * hf >= hd + hi5 + fi4 + fb + 4.0 * ei) return 0.0;
  mediump float fg = hd_df(f, g), hc = hd_df(h, c);
  bool shallow = 2.0 * fg <= hc && hd_differ(e, g) && hd_differ(d, g);
  bool steep = fg >= 2.0 * hc && hd_differ(e, c) && hd_differ(b, c);
  return shallow && steep ? 4.0 : shallow ? 2.0 : steep ? 3.0 : 1.0;
}
mediump float hd_bit(bool on, mediump float value) { return on ? value : 0.0; }
void main() {
  vec2 p = floor(gl_FragCoord.xy);
  mediump vec4 centre = hd_fetch(p);
  mediump vec3 E = centre.rgb;
  mediump vec4 Bq = hd_fetch(p + vec2(0.0, -1.0)), Dq = hd_fetch(p + vec2(-1.0, 0.0)), Fq = hd_fetch(p + vec2(1.0, 0.0));
  mediump vec4 Hq = hd_fetch(p + vec2(0.0, 1.0)), Aq = hd_fetch(p + vec2(-1.0, -1.0)), Cq = hd_fetch(p + vec2(1.0, -1.0));
  mediump vec4 Gq = hd_fetch(p + vec2(-1.0, 1.0)), Iq = hd_fetch(p + vec2(1.0, 1.0));
  mediump vec3 B = Bq.rgb, D = Dq.rgb, F = Fq.rgb, H = Hq.rgb, A = Aq.rgb, C = Cq.rgb, G = Gq.rgb, I = Iq.rgb;
  bool own = centre.a > 0.5;
  // A pixel equal to all 8 neighbours (most of any 2D frame) has nothing to smooth and no corner.
  if (!hd_differ(E, B) && !hd_differ(E, D) && !hd_differ(E, F) && !hd_differ(E, H) && !hd_differ(E, A) && !hd_differ(E, C) &&
      !hd_differ(E, G) && !hd_differ(E, I)) {
    gl_FragColor = vec4(0.0, 0.0, hd_bit(own, 16.0) / 255.0, 0.0);
    return;
  }
  mediump float eB = hd_df(E, B), eD = hd_df(E, D), eF = hd_df(E, F), eH = hd_df(E, H);
  mediump float eA = hd_df(E, A), eC = hd_df(E, C), eG = hd_df(E, G), eI = hd_df(E, I);
  // Smoothing: 2D neighbours of a 2D pixel that differ from it, but by less than u_hd_smooth (equal ones change nothing).
  mediump float smooth = 0.0;
  if (own) {
    smooth = hd_bit(Fq.a > 0.5 && eF < u_hd_smooth && hd_differ(E, F), 1.0) + hd_bit(Hq.a > 0.5 && eH < u_hd_smooth && hd_differ(E, H), 2.0) +
             hd_bit(Dq.a > 0.5 && eD < u_hd_smooth && hd_differ(E, D), 4.0) + hd_bit(Bq.a > 0.5 && eB < u_hd_smooth && hd_differ(E, B), 8.0) +
             hd_bit(Iq.a > 0.5 && eI < u_hd_smooth && hd_differ(E, I), 16.0) + hd_bit(Gq.a > 0.5 && eG < u_hd_smooth && hd_differ(E, G), 32.0) +
             hd_bit(Cq.a > 0.5 && eC < u_hd_smooth && hd_differ(E, C), 64.0) + hd_bit(Aq.a > 0.5 && eA < u_hd_smooth && hd_differ(E, A), 128.0);
  }
  // Level 0 and the structural floor: each corner faces two other colours, both at least u_hd_edge away.
  bool k0 = hd_differ(E, F) && hd_differ(E, H) && min(eF, eH) >= u_hd_edge;
  bool k1 = hd_differ(E, B) && hd_differ(E, F) && min(eB, eF) >= u_hd_edge;
  bool k2 = hd_differ(E, D) && hd_differ(E, B) && min(eD, eB) >= u_hd_edge;
  bool k3 = hd_differ(E, H) && hd_differ(E, D) && min(eH, eD) >= u_hd_edge;
  mediump float s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0, t0 = 0.0, t1 = 0.0, t2 = 0.0, t3 = 0.0;
  if (k0 || k1 || k2 || k3) {
    mediump vec3 A1 = hd_fetch(p + vec2(-1.0, -2.0)).rgb, B1 = hd_fetch(p + vec2(0.0, -2.0)).rgb;
    mediump vec3 C1 = hd_fetch(p + vec2(1.0, -2.0)).rgb, A0 = hd_fetch(p + vec2(-2.0, -1.0)).rgb;
    mediump vec3 C4 = hd_fetch(p + vec2(2.0, -1.0)).rgb, D0 = hd_fetch(p + vec2(-2.0, 0.0)).rgb;
    mediump vec3 F4 = hd_fetch(p + vec2(2.0, 0.0)).rgb, G0 = hd_fetch(p + vec2(-2.0, 1.0)).rgb;
    mediump vec3 I4 = hd_fetch(p + vec2(2.0, 1.0)).rgb, G5 = hd_fetch(p + vec2(-1.0, 2.0)).rgb;
    mediump vec3 H5 = hd_fetch(p + vec2(0.0, 2.0)).rgb, I5 = hd_fetch(p + vec2(1.0, 2.0)).rgb;
    // The side diagonals serve as f-b and h-d of one corner and h-f of another.
    mediump float FB = hd_df(F, B), BD = hd_df(B, D), DH = hd_df(D, H), HF = hd_df(H, F);
    // Each corner rotated: bottom-right, top-right, top-left, bottom-left.
    if (k0) s0 = hd_corner(E, B, C, D, F, G, H, I, I4, I5, H5, F4, FB, DH, HF, eI, eG, eC, eF, eH, t0);
    if (k1) s1 = hd_corner(E, D, A, H, B, I, F, C, C1, C4, F4, B1, BD, HF, FB, eC, eI, eA, eB, eF, t1);
    if (k2) s2 = hd_corner(E, H, G, F, D, C, B, A, A0, A1, B1, D0, DH, FB, BD, eA, eC, eG, eD, eB, t2);
    if (k3) s3 = hd_corner(E, F, I, B, H, A, D, G, G5, G0, D0, H5, HF, BD, DH, eG, eA, eI, eH, eD, t3);
    t0 *= step(0.5, s0); t1 *= step(0.5, s1); t2 *= step(0.5, s2); t3 *= step(0.5, s3);
  }
  gl_FragColor = vec4((s0 * 8.0 + s1) / 255.0, (s2 * 8.0 + s3) / 255.0,
                      (t0 + 2.0 * t1 + 4.0 * t2 + 8.0 * t3 + hd_bit(own, 16.0)) / 255.0, smooth / 255.0);
}
)";

// The reconstruction as a function of the screen position uv (0..1, top left), for a pass of its own or inside a
// screen filter's sampling function: one read of the picture per output pixel, no intermediate image.
inline constexpr const char* hd2d_sample = R"(
uniform sampler2D u_hd_rules;
uniform vec2 u_hd_out;        // the drawn size in pixels (the footprint of one fragment)
mediump vec3 hd_lin(mediump vec3 c) { return c * c; }   // gamma 2: close to the panel's, and cheap
// Coverage of corner `state` (direction axes fd, hd) at offset xy from the pixel centre, footprint w.
mediump float hd_cover(mediump float state, mediump vec2 xy, mediump vec2 fd, mediump vec2 hd, mediump float w) {
  if (state < 0.5) return 0.0;
  mediump float s = dot(xy, fd), t = dot(xy, hd);
  mediump float d = (s + t - 0.5) * 0.70710678;
  if ((state > 1.5 && state < 2.5) || state > 3.5) d = max(d, (t + 0.5 * s - 0.25) * 0.89442719);
  if (state > 2.5) d = max(d, (s + 0.5 * t - 0.25) * 0.89442719);
  return clamp(d / (w * u_hd_soft) + 0.5, 0.0, 1.0);
}
bool hd_has(mediump float mask, mediump float value) { return mod(floor(mask / value), 2.0) > 0.5; }
mediump vec4 hd2d(vec2 uv) {
  // At 2.5x every fifth fragment centre lies exactly on a native pixel edge; the nudge settles those ties the same
  // way whether uv came from gl_FragCoord or from an interpolated texture coordinate.
  vec2 nc = uv * HD_NATIVE + 0.001;
  vec2 ij = floor(nc);
  mediump vec4 rule = floor(texture2D(u_hd_rules, (ij + 0.5) / HD_NATIVE) * 255.0 + 0.5);
  bool own = rule.b >= 16.0;
  // Nothing happens at this pixel: it is shown as it is (or the 3D under it).
  if (rule.r + rule.g + rule.a < 0.5) return vec4(own ? hd_fetch(ij).rgb : hd_source(uv).rgb, 1.0);
  mediump vec2 xy = nc - ij - 0.5;
  mediump float w = HD_NATIVE.x / u_hd_out.x;
  mediump vec3 e = own ? hd_fetch(ij).rgb : hd_source(uv).rgb;
  mediump vec3 base = hd_lin(e);
  if (rule.a > 0.5) {
    // Bilinear toward the flagged neighbours on this fragment's side of the pixel centre; the rest count as the pixel.
    mediump float sx = xy.x >= 0.0 ? 1.0 : -1.0, sy = xy.y >= 0.0 ? 1.0 : -1.0;
    mediump float wx = abs(xy.x), wy = abs(xy.y);
    mediump float hb = sx > 0.0 ? 1.0 : 4.0, vb = sy > 0.0 ? 2.0 : 8.0;
    mediump float db = sy > 0.0 ? (sx > 0.0 ? 16.0 : 32.0) : (sx > 0.0 ? 64.0 : 128.0);
    mediump vec3 h = hd_has(rule.a, hb) ? hd_lin(hd_fetch(ij + vec2(sx, 0.0)).rgb) : base;
    mediump vec3 v = hd_has(rule.a, vb) ? hd_lin(hd_fetch(ij + vec2(0.0, sy)).rgb) : base;
    mediump vec3 d = hd_has(rule.a, db) ? hd_lin(hd_fetch(ij + vec2(sx, sy)).rgb) : base;
    base = mix(mix(base, h, wx), mix(v, d, wx), wy);
  }
  if (rule.r + rule.g > 0.5) {
    mediump float s0 = floor(rule.r / 8.0), s1 = rule.r - 8.0 * s0, s2 = floor(rule.g / 8.0), s3 = rule.g - 8.0 * s2;
    // Each corner's axes: right/down, up/right, left/up, down/left (texture rows grow downward).
    mediump float k0 = hd_cover(s0, xy, vec2(1.0, 0.0), vec2(0.0, 1.0), w);
    mediump float k1 = hd_cover(s1, xy, vec2(0.0, -1.0), vec2(1.0, 0.0), w);
    mediump float k2 = hd_cover(s2, xy, vec2(-1.0, 0.0), vec2(0.0, -1.0), w);
    mediump float k3 = hd_cover(s3, xy, vec2(0.0, 1.0), vec2(-1.0, 0.0), w);
    mediump float best = max(max(k0, k1), max(k2, k3));
    if (best > 0.0) {
      mediump float t = rule.b - (own ? 16.0 : 0.0);
      vec2 o = k0 == best ? (hd_has(t, 1.0) ? vec2(1.0, 0.0) : vec2(0.0, 1.0))
             : k1 == best ? (hd_has(t, 2.0) ? vec2(0.0, -1.0) : vec2(1.0, 0.0))
             : k2 == best ? (hd_has(t, 4.0) ? vec2(-1.0, 0.0) : vec2(0.0, -1.0))
                          : (hd_has(t, 8.0) ? vec2(0.0, 1.0) : vec2(-1.0, 0.0));
      mediump vec4 n = hd_fetch(ij + o);
      // A corner between two 3D pixels is the 3D's own business; otherwise 3D stands in by its high-res colour.
      if (n.a > 0.5 || own) base = mix(base, hd_lin(n.a > 0.5 ? n.rgb : hd_source(uv).rgb), best);
    }
  }
  return vec4(sqrt(base), 1.0);
}
)";

inline constexpr const char* hd2d_reconstruct = R"(
void main() { gl_FragColor = hd2d(gl_FragCoord.xy / u_hd_out); }
)";

} // namespace rgds
