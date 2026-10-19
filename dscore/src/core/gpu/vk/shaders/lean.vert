#version 450
// The lean path: polygons as fans through the hardware rasteriser, one
// instance per polygon, at 1x with the pipeline's MSAA. Vertices sit on
// pixel corners so the samples measure the polygon's true area: the DS
// span covers columns [xstart, xend] inclusive and rows [ytop, ybot)
// exclusive, so a left vertex is on its pixel's left edge and a right one on
// its pixel's right edge.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) out vec3 v_rgb;                 // 9-bit vertex colour, perspective-correct
layout(location = 1) out vec2 v_st;                  // texture coordinates in texels (12.4 / 16), perspective-correct
// RG DS: everything the fragment stage needs about its polygon, decoded once
// per vertex (the fragment stage is the cost at 2x-5x; its ALU is the limit).
// Per texture axis: clamp bounds (wide open when repeating), the flip bit
// (the size when mirroring, else 0) and size - 1.
layout(location = 2) flat out ivec4 v_ts;
layout(location = 3) flat out ivec4 v_tt;
layout(location = 4) flat out ivec4 v_tm;            // atlas corner x, y; blend mode; textured
layout(location = 5) flat out vec4 v_stb;            // texcoord range s min, t min, s max, t max (texels)
layout(location = 6) flat out vec2 v_af;             // polygon alpha, fog bit
void main() {
  uint pi = uint(gl_InstanceIndex);
  GpuPoly p = polys[pi];
  uint tri = uint(gl_VertexIndex) / 3u, k = uint(gl_VertexIndex) % 3u;
  if (tri + 2u >= p.nverts) {
    gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
    v_rgb = vec3(0); v_st = vec2(0); v_ts = ivec4(0); v_tt = ivec4(0); v_tm = ivec4(0); v_stb = vec4(0); v_af = vec2(0);
    return;
  }
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u && (pc.f.flags & DS_FF_DEBUG_NOTEX) == 0u;
  int tw = int(p.tex_w), th = int(p.tex_h), open = 0x40000000;
  bool rep_s = (p.texparam & 0x10000u) != 0u, rep_t = (p.texparam & 0x20000u) != 0u;
  bool flp_s = (p.texparam & 0x40000u) != 0u, flp_t = (p.texparam & 0x80000u) != 0u;
  v_ts = ivec4(rep_s ? -open : 0, rep_s ? open : tw - 1, rep_s && flp_s ? tw : 0, tw - 1);
  v_tt = ivec4(rep_t ? -open : 0, rep_t ? open : th - 1, rep_t && flp_t ? th : 0, th - 1);
  v_tm = ivec4(int(p.tex_offset & 0xFFFFu), int(p.tex_offset >> 16), int((p.attr >> 4) & 3u), textured ? 1 : 0);
  // The polygon's own texcoord range (Lean::submit packs it into row_base /
  // pad_ as 16-bit pairs, +32768): the fragment stage clamps to it, so an
  // edge pixel's extrapolated corner never reads a texel past the polygon.
  v_stb = vec4(float(int(p.row_base & 0xFFFFu) - 32768), float(int(p.pad_ & 0xFFFFu) - 32768),
               float(int(p.row_base >> 16) - 32768), float(int(p.pad_ >> 16) - 32768)) / 16.0;
  v_af = vec2(float((p.attr >> 16) & 0x1Fu), ((pc.f.dispcnt & 128u) != 0u && (p.attr & 0x8000u) != 0u) ? 1.0 : 0.0);
  uint vi = k == 0u ? 0u : tri + k;
  GpuVert vt = verts[p.first_vert + vi];
  float w = max(float(vt.w), 1.0);
  float cx = 0.5 * float(p.xmin + p.xmax);
  float sxf = float(vt.sx), syf = float(vt.sy);
  float ox = sxf > cx ? 1.0 : 0.0, oy = 0.0;
  // A zero-width/height polygon is a line to the DS but zero area here:
  // give it one pixel via the second/third vertex.
  if (p.xmax == p.xmin) ox = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  if (p.ybot == p.ytop) oy = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  float x = (sxf + ox) / 256.0 * 2.0 - 1.0, y = (syf + oy) / 192.0 * 2.0 - 1.0;
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  // Z-buffer: z/2^24 linear in screen space. W-buffer: vt.z is already the
  // per-polygon normalised depth the DS compares; 1/z interpolated
  // perspective-correct reproduces it, and the test runs GREATER.
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  // Depth-equal polygons (POLYGON_ATTR bit 14: decals on a surface) pass
  // within a tolerance of the pixel's depth, +-0x200 Z-buffered, +-0xFF
  // W-buffered. Here: the polygon sits that much nearer and its pipeline
  // tests less-or-equal, so a decal at, or slightly behind, its surface
  // draws. (A decal well in front would pass too; the DS fails it. Rare.)
  float zd = z;
  if ((p.attr & 0x4000u) != 0u) zd = max(z - (wbuf ? 255.0 : 512.0), 0.0);
  float zc = wbuf ? (1.0 / max(zd, 1.0)) * w : (zd / 16777215.0) * w;
  gl_Position = vec4(x * w, y * w, zc, w);
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  // Scaling by 1/16 is exact in binary floating point, so interpolating
  // texels gives the same floor as interpolating 12.4 and shifting by 4.
  v_st = vec2(float(vt.s), float(vt.t)) / 16.0;
}
