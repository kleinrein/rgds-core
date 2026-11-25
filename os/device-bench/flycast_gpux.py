# GPU experiments for Flycast's GLES renderer, switched by RGDS_X_* (see the comment added to gles.h). Applied to the
# VM build tree on top of package patches 0001-0007; backups *.pre-gpux.
import shutil, sys
from pathlib import Path

tree = Path(sys.argv[1]) / 'core/rend/gles'


def edit(name, pairs):
    src = tree / name
    backup = src.with_name(src.name + '.pre-gpux')
    if not backup.exists(): shutil.copy2(src, backup)
    s = backup.read_text()
    for old, new, count in pairs:
        assert s.count(old) == count, (name, old[:60], s.count(old))
        s = s.replace(old, new)
    src.write_text(s)


edit(
    'gles.h', [
        (
            '''		addConstant("RGDS_VTXDEPTH", rgds_vertex_depth() ? 1 : 0);
''', '''		addConstant("RGDS_VTXDEPTH", rgds_vertex_depth() ? 1 : 0);
		addConstant("RGDS_PERSP", rgds_vertex_depth() && rgds_x("PERSP") ? 1 : 0);
		addConstant("RGDS_MEDIUMP", rgds_vertex_depth() && rgds_x("PERSP") && rgds_x("MEDIUMP") ? 1 : 0);
''', 1
        ),
        (
            '''bool rgds_vertex_depth();
''', '''bool rgds_vertex_depth();
// RG DS experiments, each on with RGDS_X_<NAME>=1: PERSP (the GPU's own perspective correction: real w from the
// vertex shader instead of three divides per pixel), MEDIUMP (colours as mediump, with PERSP), CLEAR (clear colour
// at the start of a full-screen frame so the tiles are not loaded back), INVAL (drop depth and stencil at the end of
// the frame so they are not stored). RGDS_X_SKIP=opmt leaves a list out (opaque, punch-through, modifier volumes,
// translucent) to weigh it.
int rgds_x(const char *name);
''', 1
        )
    ]
)

edit(
    'gles.cpp', [
        (
            '''bool rgds_vertex_depth()
''', '''int rgds_x(const char *name)
{
	char key[64];
	snprintf(key, sizeof(key), "RGDS_X_%s", name);
	const char *e = getenv(key);
	return e != nullptr ? atoi(e) : 0;
}

bool rgds_vertex_depth()
''', 1
        ),
        (
            '''INTERPOLATION out highp vec4 vtx_base;
INTERPOLATION out highp vec4 vtx_offs;
''', '''#if RGDS_MEDIUMP == 1
#define COLPREC mediump
#else
#define COLPREC highp
#endif
INTERPOLATION out COLPREC vec4 vtx_base;
INTERPOLATION out COLPREC vec4 vtx_offs;
''', 1
        ),
        (
            '''	#if pp_Gouraud == 1 && DIV_POS_Z != 1
		vtx_base *= vpos.z;
''', '''	#if pp_Gouraud == 1 && DIV_POS_Z != 1 && RGDS_PERSP != 1
		vtx_base *= vpos.z;
''', 1
        ),
        (
            '''	#if DIV_POS_Z != 1
		vtx_uv.xy *= vpos.z;
		vpos.w = 1.0;
''', '''	#if DIV_POS_Z != 1 && RGDS_PERSP == 1
		highp float rw = 1.0 / vpos.z;
		vpos = vec4(vpos.xy * rw, clamp(depth_scale.x * vpos.z + depth_scale.y, -1.0, 1.0) * rw, rw);
	#elif DIV_POS_Z != 1
		vtx_uv.xy *= vpos.z;
		vpos.w = 1.0;
''', 1
        ),
        (
            '''INTERPOLATION in highp vec4 vtx_base;
INTERPOLATION in highp vec4 vtx_offs;
in highp vec3 vtx_uv;

lowp float fog_mode2''', '''#if RGDS_MEDIUMP == 1
#define COLPREC mediump
#else
#define COLPREC highp
#endif
INTERPOLATION in COLPREC vec4 vtx_base;
INTERPOLATION in COLPREC vec4 vtx_offs;
in highp vec3 vtx_uv;

lowp float fog_mode2''', 1
        ),
        (
            '''#if TARGET_GL == GLES2 || TARGET_GL == GL2 || DIV_POS_Z == 1
	return getPaletteEntry(texture(tex, coords.xy).FOG_CHANNEL);''',
            '''#if TARGET_GL == GLES2 || TARGET_GL == GL2 || DIV_POS_Z == 1 || RGDS_PERSP == 1
	return getPaletteEntry(texture(tex, coords.xy).FOG_CHANNEL);''', 1
        ),
        (
            '''#if TARGET_GL != GLES2 && TARGET_GL != GL2 && DIV_POS_Z != 1
	coords.xy /= coords.z;''', '''#if TARGET_GL != GLES2 && TARGET_GL != GL2 && DIV_POS_Z != 1 && RGDS_PERSP != 1
	coords.xy /= coords.z;''', 1
        ),
        (
            '''#if TARGET_GL == GLES2
#define depth gl_FragCoord.w
''', '''#if TARGET_GL == GLES2 || RGDS_PERSP == 1
#define depth gl_FragCoord.w
''', 1
        ),
        (
            '''	highp vec4 color = vtx_base;
	highp vec4 offset = vtx_offs;
	#if pp_Gouraud == 1 && TARGET_GL != GLES2 && DIV_POS_Z != 1
''', '''	COLPREC vec4 color = vtx_base;
	COLPREC vec4 offset = vtx_offs;
	#if pp_Gouraud == 1 && TARGET_GL != GLES2 && DIV_POS_Z != 1 && RGDS_PERSP != 1
''', 1
        ),
        (
            '''		  #if TARGET_GL == GLES2 || TARGET_GL == GL2 || DIV_POS_Z == 1
			lowp vec4 texcol = texture(tex, vtx_uv.xy);''',
            '''		  #if TARGET_GL == GLES2 || TARGET_GL == GL2 || DIV_POS_Z == 1 || RGDS_PERSP == 1
			lowp vec4 texcol = texture(tex, vtx_uv.xy);''', 1
        ),
        (
            '''	if (is_rtt || gl.rendContext->clearFramebuffer)
		glClear(GL_COLOR_BUFFER_BIT);
''', '''	static const int xClear = rgds_x("CLEAR"), xInval = rgds_x("INVAL");
	bool colourCleared = is_rtt || gl.rendContext->clearFramebuffer;
	if (colourCleared)
		glClear(GL_COLOR_BUFFER_BIT);
''', 1
        ),
        (
            '''	if (ShaderUniforms.base_clipping.enabled) {
		glcache.Scissor(ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y,
				ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
		glcache.Enable(GL_SCISSOR_TEST);
	}

	DrawStrips();
''', '''	if (xClear && !colourCleared && !ShaderUniforms.base_clipping.enabled)
		glClear(GL_COLOR_BUFFER_BIT);
	if (ShaderUniforms.base_clipping.enabled) {
		glcache.Scissor(ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y,
				ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
		glcache.Enable(GL_SCISSOR_TEST);
	}

	DrawStrips();
	if (xInval && !is_rtt && !config::EmulateFramebuffer)
	{
		typedef void (*InvalidateProc)(GLenum, GLsizei, const GLenum *);
		static const InvalidateProc invalidate = (InvalidateProc)hw_render.get_proc_address("glInvalidateFramebuffer");
		const GLenum gone[] = { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
		if (invalidate != nullptr)
			invalidate(GL_FRAMEBUFFER, 2, gone);
	}
''', 1
        )
    ]
)

edit(
    'gldraw.cpp', [
        (
            '''		rgds_stats_mark(RS_OP);
		DrawList<ListType_Opaque,false>(''', '''		rgds_stats_mark(RS_OP);
		static const int xSkip = [] {
			const char *e = getenv("RGDS_X_SKIP");
			int mask = 0;
			for (; e != nullptr && *e != 0; e++)
				mask |= *e == 'o' ? 1 : *e == 'p' ? 2 : *e == 'm' ? 4 : *e == 't' ? 8 : 0;
			return mask;
		}();
		if (!(xSkip & 1))
		DrawList<ListType_Opaque,false>(''', 1
        ),
        (
            '''		rgds_stats_mark(RS_PT);
		DrawList<ListType_Punch_Through,false>(''', '''		rgds_stats_mark(RS_PT);
		if (!(xSkip & 2))
		DrawList<ListType_Punch_Through,false>(''', 1
        ),
        (
            '''		if (config::ModifierVolumes)
			DrawModVols(previous_pass.mvo_count''', '''		if (config::ModifierVolumes && !(xSkip & 4))
			DrawModVols(previous_pass.mvo_count''', 1
        ),
        (
            '''		rgds_stats_mark(RS_TR);
		{
			if (current_pass.autosort)''', '''		rgds_stats_mark(RS_TR);
		if (!(xSkip & 8))
		{
			if (current_pass.autosort)''', 1
        )
    ]
)
print('applied')
