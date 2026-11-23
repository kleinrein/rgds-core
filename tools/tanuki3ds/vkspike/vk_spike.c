// Tanuki3DS Vulkan feasibility spike: the same synthetic 3DS frame through desktop GL 3.3
// (EGL surfaceless, Tanuki's call pattern) and through Vulkan (one ICD loaded directly, no
// loader), with Tanuki's own generated shaders. Measures shader/pipeline build times and the
// CPU cost per draw of both APIs, checks the two pictures against each other, and tries the
// dma-buf hand-off of a Vulkan image to GL (how frames would reach the present thread).
//
//   vk_spike [--mode gl|vk|both] [--list shaders.list] [--vert gpu.vert] [--draws 400]
//            [--frames 300] [--progs 40] [--tiny] [--texupdates 4] [--cache FILE]
//            [--icd libvulkan_x.so] [--shots DIR] [--dmabuf] [--compile-only]

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include "video/shadergen_fs.h"
#include "vk_glsl.h"

bool g_infologs;

// ---------------------------------------------------------------- common

#define FBW 240 // the 3DS top screen as the PICA draws it: 240 wide, 400 tall
#define FBH 400
#define GRID 8
#define NVERT ((GRID + 1) * (GRID + 1))
#define NIDX (GRID * GRID * 6)
#define NTEX 24
#define TEXSZ 128
#define NVARIANT 4
#define SLOT_BYTES (24u << 20)

static int o_draws = 400, o_frames = 300, o_progs = 40, o_tiny, o_texupdates = 4, o_dmabuf, o_compile_only, o_warm = 20,
           o_ibuf_exact = 1, o_nonindexed;
static const char *o_list, *o_vert = "gpu.vert", *o_cache, *o_shots, *o_mode = "both",
                           *o_icd = "/usr/lib/rgds-panvk/libvulkan_panfrost.so";

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static double cpu_ms(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (uint32_t)(rs >> 16);
}
static float frnd(void) { return (rnd() & 0xffff) / 65535.0f; }

static char* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* p = malloc(sz + 1);
    if (fread(p, 1, sz, f) != (size_t)sz) sz = 0;
    p[sz] = 0;
    fclose(f);
    if (n) *n = sz;
    return p;
}

// GPU time per engine from the panfrost kernel driver (drm-engine-* in fdinfo), all DRM
// fds of this process summed; 0 where the driver has no such lines (llvmpipe/lavapipe)
static void gpu_engine_ns(uint64_t* frag, uint64_t* vt) {
    *frag = *vt = 0;
    DIR* d = opendir("/proc/self/fdinfo");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[64], buf[4096];
        snprintf(path, sizeof path, "/proc/self/fdinfo/%s", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        char* l;
        if ((l = strstr(buf, "drm-engine-fragment:"))) *frag += strtoull(l + 20, 0, 10);
        if ((l = strstr(buf, "drm-engine-vertex-tiler:"))) *vt += strtoull(l + 24, 0, 10);
    }
    closedir(d);
}

static void list_threads(const char* tag) {
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent* e;
    fprintf(stderr, "%s threads:", tag);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[64], name[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE* f = fopen(path, "r");
        if (f) {
            if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
            fclose(f);
        }
        fprintf(stderr, " %s", name);
    }
    closedir(d);
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------- shaders

typedef struct {
    char* src;
    uint64_t hash;
} Src;
static Src* g_vs;
static int g_nvs;
static Src* g_fs;
static int g_nfs;
static struct {
    int v, f;
}* g_pairs;
static int g_npairs;
static char* g_drawvs; // the vertex shader the draws use (hostshaders/gpu.vert)

static int find_src(Src* s, int n, uint64_t h) {
    for (int i = 0; i < n; i++)
        if (s[i].hash == h) return i;
    return -1;
}

// Tanuki's shaders/<rom>.list: header, then 'V'/'F' hash len text and 'P' vhash fhash
static void load_list(const char* path) {
    size_t n;
    char* b = read_file(path, &n);
    if (!b || n < 8) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    size_t p = 8;
    while (p < n) {
        int type = b[p++];
        if (type == 'V' || type == 'F') {
            uint64_t h;
            uint32_t len;
            if (p + 12 > n) break;
            memcpy(&h, b + p, 8);
            memcpy(&len, b + p + 8, 4);
            p += 12;
            if (p + len > n) break;
            char* s = malloc(len + 1);
            memcpy(s, b + p, len);
            s[len] = 0;
            p += len;
            Src** tab = type == 'V' ? &g_vs : &g_fs;
            int* cnt = type == 'V' ? &g_nvs : &g_nfs;
            *tab = realloc(*tab, (*cnt + 1) * sizeof(Src));
            (*tab)[(*cnt)++] = (Src){s, h};
        } else if (type == 'P') {
            uint64_t h[2];
            if (p + 16 > n) break;
            memcpy(h, b + p, 16);
            p += 16;
            int v = find_src(g_vs, g_nvs, h[0]), f = find_src(g_fs, g_nfs, h[1]);
            if (v >= 0 && f >= 0) {
                g_pairs = realloc(g_pairs, (g_npairs + 1) * sizeof g_pairs[0]);
                g_pairs[g_npairs].v = v;
                g_pairs[g_npairs].f = f;
                g_npairs++;
            }
        } else {
            break;
        }
    }
    fprintf(stderr, "list %s: %d vertex, %d fragment sources, %d programs\n", path, g_nvs, g_nfs, g_npairs);
}

static uint32_t* glsl_to_spirv(const char* src, int stage, size_t* words, double* ms);
// without a list: fragment shaders from Tanuki's generator over random configurations
static void synth_fs(int n) {
    for (int i = 0; i < n; i++) {
        FragConfig c = {};
        static const int srcs[] = {TEVSRC_COLOR, TEVSRC_TEX0,     TEVSRC_TEX1,
                                   TEVSRC_TEX2,  TEVSRC_CONSTANT, TEVSRC_PREVIOUS};
        int stages = 1 + rnd() % 4;
        for (int s = 0; s < 6; s++) {
            if (s < stages) {
                c.tev[s].rgb.src0 = srcs[rnd() % 6];
                c.tev[s].rgb.src1 = srcs[rnd() % 6];
                c.tev[s].rgb.src2 = srcs[rnd() % 6];
                c.tev[s].rgb.combiner = rnd() % 5;
                c.tev[s].a = c.tev[s].rgb;
            } else {
                c.tev[s].rgb.src0 = c.tev[s].a.src0 = TEVSRC_PREVIOUS;
            }
            c.tev[s].rgb.scale = c.tev[s].a.scale = 1;
        }
        c.texconfig.tex0enable = 1;
        c.texconfig.tex1enable = rnd() & 1;
        c.texconfig.tex2enable = rnd() & 1;
        c.lightDisable = (rnd() % 4) != 0;
        if (!c.lightDisable) {
            c.numlights = 1 + rnd() % 2;
            c.lightPerm = 0x76543210;
            c.lconfig1.w = 0xffffffff;
        }
        c.alphatest = rnd() & 1;
        c.alphafunc = 4 + rnd() % 2;
        char* s = shader_gen_fs(&c);
        // keep only what compiles (a random configuration can be one no game makes)
        char* f = vkglsl_fixup(s, 1);
        size_t nw;
        double ms = 0;
        uint32_t* spv = f ? glsl_to_spirv(f, 1, &nw, &ms) : 0;
        free(f);
        if (!spv) {
            free(s);
            i--;
            continue;
        }
        free(spv);
        g_fs = realloc(g_fs, (g_nfs + 1) * sizeof(Src));
        g_fs[g_nfs++] = (Src){s, i};
    }
}

static uint32_t* glsl_to_spirv(const char* src, int stage, size_t* words, double* ms) {
    double t0 = now_ms();
    glslang_input_t in = {
        .language = GLSLANG_SOURCE_GLSL,
        .stage = stage ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX,
        .client = GLSLANG_CLIENT_VULKAN,
        .client_version = GLSLANG_TARGET_VULKAN_1_0,
        .target_language = GLSLANG_TARGET_SPV,
        .target_language_version = GLSLANG_TARGET_SPV_1_0,
        .code = src,
        .default_version = 450,
        .default_profile = GLSLANG_NO_PROFILE,
        .messages = GLSLANG_MSG_DEFAULT_BIT,
        .resource = glslang_default_resource(),
    };
    glslang_shader_t* sh = glslang_shader_create(&in);
    uint32_t* out = 0;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        fprintf(stderr, "glslang: %s\n", glslang_shader_get_info_log(sh));
        glslang_shader_delete(sh);
        return 0;
    }
    glslang_program_t* pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        glslang_program_SPIRV_generate(pr, in.stage);
        *words = glslang_program_SPIRV_get_size(pr);
        out = malloc(*words * 4);
        glslang_program_SPIRV_get(pr, out);
    } else {
        fprintf(stderr, "glslang link: %s\n", glslang_program_get_info_log(pr));
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    *ms += now_ms() - t0;
    return out;
}

// ---------------------------------------------------------------- the scene

typedef struct {
    float pos[4], nq[4];
} VA; // binding 0
typedef struct {
    float color[4], tc0[2], tc1[2], tc2[2], tcw, view[3], pad[2];
} VB; // binding 1

typedef struct {
    int prog, variant, tex[3];
    FragUniforms fu;
    VA va[NVERT];
    VB vb[NVERT];
    VA xa[NIDX]; // the same triangles without indices (--nonindexed)
    VB xb[NIDX];
} Draw;
static Draw* g_scene;
static uint16_t g_idx[NIDX];
static uint32_t* g_texels[NTEX];
static uint16_t g_lightlut[32][256], g_foglut[129], g_ptnoise[128], g_ptmap[128][2];
static uint32_t g_proctex[256];
static uint32_t g_upd[64][32 * 32]; // texture update payloads

static void scene_build(void) {
    rs = 0x1234567;
    g_scene = calloc(o_draws, sizeof(Draw));
    for (int y = 0, k = 0; y < GRID; y++)
        for (int x = 0; x < GRID; x++) {
            int a = y * (GRID + 1) + x;
            uint16_t q[6] = {a, a + 1, a + GRID + 1, a + 1, a + GRID + 2, a + GRID + 1};
            memcpy(g_idx + k, q, sizeof q);
            k += 6;
        }
    for (int d = 0; d < o_draws; d++) {
        Draw* w = &g_scene[d];
        w->prog = (d / 3) % o_progs;
        w->variant = (w->prog + (d & 1)) % NVARIANT;
        w->tex[0] = (d * 7) % NTEX;
        w->tex[1] = (d * 3 + 1) % NTEX;
        w->tex[2] = (d + 5) % NTEX;
        float* f = (float*)&w->fu;
        for (size_t i = 0; i < sizeof w->fu / 4; i++) f[i] = frnd();
        w->fu.alpharef = 0.1f;
        for (int l = 0; l < 8; l++) {
            w->fu.light[l].attn_bias = 0;
            w->fu.light[l].attn_scale = 1;
        }
        // a patch of the screen, about 45x45 pixels on average
        float cx = frnd() * 2 - 1, cy = frnd() * 2 - 1, sx = 0.1f + frnd() * 0.35f, sy = 0.06f + frnd() * 0.2f,
              z = -(0.1f + 0.8f * frnd());
        float col[4] = {frnd(), frnd(), frnd(), 0.4f + 0.6f * frnd()};
        for (int y = 0; y <= GRID; y++)
            for (int x = 0; x <= GRID; x++) {
                int i = y * (GRID + 1) + x;
                float u = (float)x / GRID, v = (float)y / GRID;
                VA* a = &w->va[i];
                VB* b = &w->vb[i];
                a->pos[0] = cx + (u - 0.5f) * 2 * sx;
                a->pos[1] = cy + (v - 0.5f) * 2 * sy;
                a->pos[2] = z + 0.05f * u;
                a->pos[3] = 1;
                float ang = u * 0.7f;
                a->nq[0] = sinf(ang) * 0.3f;
                a->nq[1] = sinf(ang) * 0.6f;
                a->nq[2] = 0;
                a->nq[3] = cosf(ang);
                memcpy(b->color, col, sizeof col);
                b->color[0] *= 0.6f + 0.4f * u;
                b->tc0[0] = u * 2;
                b->tc0[1] = v * 2;
                b->tc1[0] = v;
                b->tc1[1] = u;
                b->tc2[0] = u * 0.5f;
                b->tc2[1] = v * 0.5f + 0.25f;
                b->tcw = 1;
                b->view[0] = u - 0.5f;
                b->view[1] = v - 0.5f;
                b->view[2] = 1.5f;
            }
    }
    for (int d = 0; d < o_draws; d++)
        for (int i = 0; i < NIDX; i++) {
            g_scene[d].xa[i] = g_scene[d].va[g_idx[i]];
            g_scene[d].xb[i] = g_scene[d].vb[g_idx[i]];
        }
    for (int t = 0; t < NTEX; t++) {
        g_texels[t] = malloc(TEXSZ * TEXSZ * 4);
        uint32_t c0 = rnd() | 0xff000000, c1 = rnd() | 0x80000000;
        for (int i = 0; i < TEXSZ * TEXSZ; i++) {
            int x = i % TEXSZ, y = i / TEXSZ;
            g_texels[t][i] = ((x >> 3) ^ (y >> 3)) & 1 ? c0 : c1 ^ (x * 0x10101);
        }
    }
    for (int l = 0; l < 32; l++)
        for (int i = 0; i < 256; i++) g_lightlut[l][i] = (uint16_t)(i * 257 * (l % 3 + 1) / 3);
    for (int i = 0; i < 129; i++) g_foglut[i] = (uint16_t)(i * 500);
    for (int i = 0; i < 128; i++) {
        g_ptnoise[i] = rnd();
        g_ptmap[i][0] = i * 512;
        g_ptmap[i][1] = 65535 - i * 512;
    }
    for (int i = 0; i < 256; i++) g_proctex[i] = rnd() | 0xff000000;
    for (int u = 0; u < 64; u++)
        for (int i = 0; i < 32 * 32; i++) g_upd[u][i] = rnd() | 0xff000000;
}

// what each state variant means (the PICA's usual cases); blend off = logic op copy
static const struct {
    int blend; // 0 off, 1 alpha, 2 add
    int depthwrite, cull;
} variants[NVARIANT] = {{0, 1, 0}, {1, 0, 0}, {2, 0, 1}, {0, 1, 1}};

typedef struct {
    const char* api;
    double build_ms, spirv_ms, frame_wall, frame_thread, frame_proc, gpu_frag, gpu_vt;
    int frames, ok;
    uint8_t* shot;
} Result;

static void tex_update_rect(int frame, int k, int* t, int* x, int* y) {
    *t = (frame * 5 + k * 11) % NTEX;
    *x = ((frame + k) * 32) % TEXSZ;
    *y = ((frame * 3 + k) * 32) % TEXSZ;
}

// ---------------------------------------------------------------- GL

static const GLenum blend_src[] = {GL_ONE, GL_SRC_ALPHA, GL_ONE},
                    blend_dst[] = {GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ONE};

static GLuint gl_compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, 0);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, 0, log);
        fprintf(stderr, "GL compile: %s\n", log);
        return 0;
    }
    return sh;
}
static GLuint gl_link(GLuint vs, GLuint fs, bool hw) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, 0, log);
        fprintf(stderr, "GL link: %s\n", log);
        return 0;
    }
    glUseProgram(p);
    static const char* names[] = {"tex0",      "tex0shadow", "tex1",          "tex2",           "tex3",
                                  "lightLuts", "fogLut",     "proctexMapLut", "proctexNoiseLut"};
    static const int units[] = {0, 0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 9; i++) glUniform1i(glGetUniformLocation(p, names[i]), units[i]);
    if (hw) {
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "VertUniforms"), 0);
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "FreecamUniforms"), 3);
    }
    GLuint fi = glGetUniformBlockIndex(p, "FragUniforms");
    if (fi != GL_INVALID_INDEX) glUniformBlockBinding(p, fi, 2);
    return p;
}

static void gl_tex(GLenum target, GLuint* t, GLint ifmt, int w, int h, GLenum fmt, GLenum ty, const void* data,
                   GLint wrap) {
    glGenTextures(1, t);
    glBindTexture(target, *t);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
    if (target == GL_TEXTURE_1D) glTexImage1D(target, 0, ifmt, w, 0, fmt, ty, data);
    else glTexImage2D(target, 0, ifmt, w, h, 0, fmt, ty, data);
}

static EGLDisplay g_egl;
static EGLContext g_eglctx;
static bool gl_up(void) {
    if (g_eglctx) return true;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_egl = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, 0) : EGL_NO_DISPLAY;
    if (!g_egl || !eglInitialize(g_egl, 0, 0)) {
        fprintf(stderr, "EGL: no surfaceless display\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {
        EGL_CONTEXT_MAJOR_VERSION,           3,       EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    g_eglctx = eglCreateContext(g_egl, (EGLConfig)0, EGL_NO_CONTEXT, ca);
    if (!g_eglctx || !eglMakeCurrent(g_egl, EGL_NO_SURFACE, EGL_NO_SURFACE, g_eglctx)) {
        fprintf(stderr, "EGL: no GL 3.3 core context (0x%x)\n", eglGetError());
        return false;
    }
    gladLoadGLLoader((GLADloadproc)eglGetProcAddress);
    fprintf(stderr, "GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

static void run_gl(Result* r) {
    r->api = "GL";
    if (!gl_up()) return;
    // programs
    double t0 = now_ms();
    GLuint vs = gl_compile(GL_VERTEX_SHADER, g_drawvs);
    GLuint* progs = calloc(o_progs, sizeof(GLuint));
    for (int i = 0; i < o_progs; i++) {
        GLuint fs = gl_compile(GL_FRAGMENT_SHADER, g_fs[i % g_nfs].src);
        progs[i] = fs ? gl_link(vs, fs, false) : 0;
        if (!progs[i]) return;
    }
    glFinish();
    r->build_ms = now_ms() - t0;
    // the list's own pairs (hardware vertex shaders): build time only
    if (g_npairs) {
        t0 = now_ms();
        GLuint* vsh = calloc(g_nvs, sizeof(GLuint));
        int n = g_npairs < o_progs ? g_npairs : o_progs;
        for (int i = 0; i < n; i++) {
            int v = g_pairs[i].v;
            if (!vsh[v]) vsh[v] = gl_compile(GL_VERTEX_SHADER, g_vs[v].src);
            GLuint fs = gl_compile(GL_FRAGMENT_SHADER, g_fs[g_pairs[i].f].src);
            if (vsh[v] && fs) gl_link(vsh[v], fs, true);
        }
        glFinish();
        fprintf(stderr, "GL: %d list programs (hw vertex shaders) built in %.0f ms\n", n, now_ms() - t0);
    }
    if (o_compile_only) {
        r->ok = 1;
        return;
    }

    GLuint fbo, color, depth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl_tex(GL_TEXTURE_2D, &color, GL_RGBA8, FBW, FBH, GL_RGBA, GL_UNSIGNED_BYTE, 0, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, FBW, FBH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);

    GLuint tex[NTEX], lightlut, foglut, ptmap, ptnoise, proctex;
    for (int t = 0; t < NTEX; t++)
        gl_tex(GL_TEXTURE_2D, &tex[t], GL_RGBA8, TEXSZ, TEXSZ, GL_RGBA, GL_UNSIGNED_BYTE, g_texels[t], GL_REPEAT);
    glGenTextures(1, &lightlut);
    glBindTexture(GL_TEXTURE_1D_ARRAY, lightlut);
    glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_R16, 256, 32, 0, GL_RED, GL_UNSIGNED_SHORT, g_lightlut);
    gl_tex(GL_TEXTURE_1D, &foglut, GL_R16, 129, 1, GL_RED, GL_UNSIGNED_SHORT, g_foglut, GL_CLAMP_TO_EDGE);
    gl_tex(GL_TEXTURE_1D, &ptmap, GL_RG16, 128, 1, GL_RG, GL_UNSIGNED_SHORT, g_ptmap, GL_CLAMP_TO_EDGE);
    gl_tex(GL_TEXTURE_1D, &ptnoise, GL_R16, 128, 1, GL_RED, GL_UNSIGNED_SHORT, g_ptnoise, GL_MIRRORED_REPEAT);
    gl_tex(GL_TEXTURE_1D, &proctex, GL_RGBA8, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, g_proctex, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_1D, proctex);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_1D_ARRAY, lightlut);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_1D, foglut);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_1D, ptmap);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_1D, ptnoise);

    // one persistent stream like Tanuki's g_stream/g_vstream, two halves for two frames
    GLuint vao, stream;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &stream);
    glBindBuffer(GL_ARRAY_BUFFER, stream);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream);
    const GLbitfield fl = GL_MAP_WRITE_BIT | 0x40 | 0x80;
    void (*bufferStorage)(GLenum, GLsizeiptr, const void*, GLbitfield) = (void*)eglGetProcAddress("glBufferStorage");
    bufferStorage(GL_ARRAY_BUFFER, 2 * SLOT_BYTES, 0, fl);
    uint8_t* sp = glMapBufferRange(GL_ARRAY_BUFFER, 0, 2 * SLOT_BYTES, fl);
    GLint ualign = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ualign);
    for (int a = 0; a < 8; a++) glEnableVertexAttribArray(a);

    GLsync fence[2] = {};
    uint64_t f0, v0, f1, v1;
    double w0 = 0, c0 = 0, p0 = 0;
    for (int frame = 0; frame < o_frames + o_warm; frame++) {
        if (frame == o_warm) {
            glFinish();
            gpu_engine_ns(&f0, &v0);
            w0 = now_ms();
            c0 = cpu_ms(CLOCK_THREAD_CPUTIME_ID);
            p0 = cpu_ms(CLOCK_PROCESS_CPUTIME_ID);
        }
        int slot = frame & 1;
        if (fence[slot]) {
            glClientWaitSync(fence[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
            glDeleteSync(fence[slot]);
            fence[slot] = 0;
        }
        size_t off = slot * SLOT_BYTES;
        for (int k = 0; k < o_texupdates; k++) {
            int t, x, y;
            tex_update_rect(frame, k, &t, &x, &y);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex[t]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, g_upd[(frame + k) & 63]);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, FBW, FBH);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glDepthMask(1);
        glClearColor(0.1f, 0.2f, 0.3f, 1);
        glClearDepth(1);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        if (o_tiny) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 1, 1);
        }
        int curprog = -1, curvar = -1, bound[3] = {-1, -1, -1}, unit = -1;
        for (int d = 0; d < o_draws; d++) {
            Draw* w = &g_scene[d];
            // the main thread's copy of the guest vertex data
            size_t va = off;
            memcpy(sp + va, w->va, sizeof w->va);
            size_t vb = va + sizeof w->va;
            memcpy(sp + vb, w->vb, sizeof w->vb);
            size_t io = vb + sizeof w->vb;
            memcpy(sp + io, g_idx, sizeof g_idx);
            size_t uo = (io + sizeof g_idx + ualign - 1) & ~(size_t)(ualign - 1);
            memcpy(sp + uo, &w->fu, sizeof w->fu);
            off = uo + sizeof w->fu;

            if (w->prog != curprog) {
                glUseProgram(progs[w->prog]);
                curprog = w->prog;
                // Tanuki keeps these per program and sends them on change only
                GLint p = progs[w->prog];
                static float sent[1024];
                if (sent[p] != -1.0f) {
                    glUniform1f(glGetUniformLocation(p, "depthOffset"), 0);
                    glUniform1f(glGetUniformLocation(p, "depthScale"), -1);
                    glUniform1i(glGetUniformLocation(p, "depthWBuffer"), 0);
                    sent[p] = -1.0f;
                }
            }
            if (w->variant != curvar) {
                int b = variants[w->variant].blend;
                if (b) {
                    glDisable(GL_COLOR_LOGIC_OP);
                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(blend_src[b], blend_dst[b], blend_src[b], blend_dst[b]);
                } else {
                    glDisable(GL_BLEND);
                    glEnable(GL_COLOR_LOGIC_OP);
                    glLogicOp(GL_COPY);
                }
                glDepthMask(variants[w->variant].depthwrite);
                if (variants[w->variant].cull) {
                    glEnable(GL_CULL_FACE);
                    glCullFace(GL_BACK);
                } else {
                    glDisable(GL_CULL_FACE);
                }
                curvar = w->variant;
            }
            for (int u = 0; u < 3; u++) {
                if (bound[u] == w->tex[u]) continue;
                if (unit != u) glActiveTexture(GL_TEXTURE0 + u);
                unit = u;
                glBindTexture(GL_TEXTURE_2D, tex[w->tex[u]]);
                bound[u] = w->tex[u];
            }
            glBindBufferRange(GL_UNIFORM_BUFFER, 2, stream, uo, sizeof w->fu);
            glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(VA), (void*)(va + 0));
            glVertexAttribPointer(6, 4, GL_FLOAT, 0, sizeof(VA), (void*)(va + 16));
            glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(VB), (void*)(vb + 0));
            glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(VB), (void*)(vb + 16));
            glVertexAttribPointer(3, 2, GL_FLOAT, 0, sizeof(VB), (void*)(vb + 24));
            glVertexAttribPointer(4, 2, GL_FLOAT, 0, sizeof(VB), (void*)(vb + 32));
            glVertexAttribPointer(5, 1, GL_FLOAT, 0, sizeof(VB), (void*)(vb + 40));
            glVertexAttribPointer(7, 3, GL_FLOAT, 0, sizeof(VB), (void*)(vb + 44));
            glDrawRangeElementsBaseVertex(GL_TRIANGLES, 0, NVERT - 1, NIDX, GL_UNSIGNED_SHORT, (void*)io, 0);
        }
        fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        if (frame == 0 && o_shots) {
            r->shot = malloc(FBW * FBH * 4);
            glReadPixels(0, 0, FBW, FBH, GL_RGBA, GL_UNSIGNED_BYTE, r->shot);
        }
    }
    glFinish();
    r->frames = o_frames;
    r->frame_wall = (now_ms() - w0) / o_frames;
    r->frame_thread = (cpu_ms(CLOCK_THREAD_CPUTIME_ID) - c0) / o_frames;
    r->frame_proc = (cpu_ms(CLOCK_PROCESS_CPUTIME_ID) - p0) / o_frames;
    gpu_engine_ns(&f1, &v1);
    r->gpu_frag = (f1 - f0) / 1e6 / o_frames;
    r->gpu_vt = (v1 - v0) / 1e6 / o_frames;
    list_threads("GL");
    r->ok = 1;
}

// ---------------------------------------------------------------- Vulkan

#define VKF(x) static PFN_vk##x vk##x
VKF(GetInstanceProcAddr);
VKF(CreateInstance);
VKF(EnumeratePhysicalDevices);
VKF(GetPhysicalDeviceProperties);
VKF(GetPhysicalDeviceFeatures);
VKF(GetPhysicalDeviceMemoryProperties);
VKF(GetPhysicalDeviceQueueFamilyProperties);
VKF(GetPhysicalDeviceFormatProperties);
VKF(EnumerateDeviceExtensionProperties);
VKF(EnumerateInstanceExtensionProperties);
VKF(CreateDevice);
VKF(GetDeviceProcAddr);
VKF(GetDeviceQueue);
VKF(CreateBuffer);
VKF(GetBufferMemoryRequirements);
VKF(AllocateMemory);
VKF(BindBufferMemory);
VKF(MapMemory);
VKF(CreateImage);
VKF(GetImageMemoryRequirements);
VKF(BindImageMemory);
VKF(CreateImageView);
VKF(CreateSampler);
VKF(CreateRenderPass);
VKF(CreateFramebuffer);
VKF(CreateShaderModule);
VKF(CreatePipelineCache);
VKF(GetPipelineCacheData);
VKF(CreateGraphicsPipelines);
VKF(CreatePipelineLayout);
VKF(CreateDescriptorSetLayout);
VKF(CreateDescriptorPool);
VKF(AllocateDescriptorSets);
VKF(UpdateDescriptorSets);
VKF(CreateCommandPool);
VKF(AllocateCommandBuffers);
VKF(ResetCommandPool);
VKF(BeginCommandBuffer);
VKF(EndCommandBuffer);
VKF(CmdBeginRenderPass);
VKF(CmdEndRenderPass);
VKF(CmdBindPipeline);
VKF(CmdSetViewport);
VKF(CmdSetScissor);
VKF(CmdBindDescriptorSets);
VKF(CmdPushConstants);
VKF(CmdBindVertexBuffers);
VKF(CmdBindIndexBuffer);
VKF(CmdDrawIndexed);
VKF(CmdDraw);
VKF(CmdPipelineBarrier);
VKF(CmdCopyBufferToImage);
VKF(CmdCopyImageToBuffer);
VKF(CreateFence);
VKF(WaitForFences);
VKF(ResetFences);
VKF(QueueSubmit);
VKF(QueueWaitIdle);
VKF(DeviceWaitIdle);
VKF(GetImageSubresourceLayout);
static PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;

static VkInstance vi;
static VkPhysicalDevice vpd;
static VkDevice vd;
static VkQueue vq;
static VkPhysicalDeviceMemoryProperties vmem;
static VkPhysicalDeviceProperties vprops;

#define CK(x)                                                                                                          \
    do {                                                                                                               \
        VkResult _r = (x);                                                                                             \
        if (_r != VK_SUCCESS) {                                                                                        \
            fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, _r);                                            \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vmem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vmem.memoryTypes[i].propertyFlags & want) == want) return i;
    return -1;
}

typedef struct {
    VkBuffer b;
    VkDeviceMemory m;
    uint8_t* p;
} Buf;
static bool buf_make(Buf* b, VkDeviceSize size, VkBufferUsageFlags use) {
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = use};
    if (vkCreateBuffer(vd, &ci, 0, &b->b)) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vd, b->b, &mr);
    int t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    if (t < 0 || vkAllocateMemory(vd, &ai, 0, &b->m)) return false;
    vkBindBufferMemory(vd, b->b, b->m, 0);
    vkMapMemory(vd, b->m, 0, VK_WHOLE_SIZE, 0, (void**)&b->p);
    return true;
}

typedef struct {
    VkImage i;
    VkDeviceMemory m;
    VkImageView v;
} Img;
static bool img_make(Img* im, VkImageType type, VkFormat fmt, int w, int h, int layers, VkImageUsageFlags use,
                     VkImageAspectFlags aspect) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = type,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = layers,
                            .samples = 1,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = use};
    if (vkCreateImage(vd, &ci, 0, &im->i)) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vd, im->i, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(vd, &ai, 0, &im->m)) return false;
    vkBindImageMemory(vd, im->i, im->m, 0);
    VkImageViewType vt = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                                                  : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->i, .viewType = vt, .format = fmt,
                                .subresourceRange = {aspect, 0, 1, 0, layers}};
    return !vkCreateImageView(vd, &vc, 0, &im->v);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, int layers, VkImageLayout from,
                    VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss,
                    VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = sa,
                              .dstAccessMask = da,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = img,
                              .subresourceRange = {aspect, 0, 1, 0, layers}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}

static bool has_ext(VkExtensionProperties* e, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

static const char* want_exts[] = {
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
    "VK_KHR_image_format_list",
    "VK_KHR_bind_memory2",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_maintenance1",
    "VK_KHR_external_memory",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_fence_fd",
    "VK_KHR_dedicated_allocation",
};
static const char* report_exts[] = {
    "VK_KHR_dynamic_rendering",
    "VK_EXT_extended_dynamic_state",
    "VK_EXT_extended_dynamic_state2",
    "VK_EXT_extended_dynamic_state3",
    "VK_KHR_push_descriptor",
    "VK_EXT_graphics_pipeline_library",
    "VK_EXT_shader_object",
    "VK_EXT_depth_clip_control",
    "VK_EXT_custom_border_color",
    "VK_KHR_timeline_semaphore",
    "VK_EXT_pipeline_creation_cache_control",
    "VK_KHR_display",
    "VK_KHR_swapchain",
    "VK_EXT_descriptor_buffer",
    "VK_KHR_dynamic_rendering_local_read",
    "VK_EXT_shader_demote_to_helper_invocation",
};

static bool vk_up(void) {
    if (vd) return true;
    void* lib = dlopen(o_icd, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "Vulkan: %s\n", dlerror());
        return false;
    }
    PFN_vkGetInstanceProcAddr icd = (void*)dlsym(lib, "vk_icdGetInstanceProcAddr");
    PFN_vkVoidFunction (*neg)(uint32_t*) = (void*)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) {
        uint32_t v = 5;
        neg(&v);
    }
    if (!icd) icd = (void*)dlsym(lib, "vkGetInstanceProcAddr");
    vkGetInstanceProcAddr = icd;
#define GI(x) vk##x = (PFN_vk##x)vkGetInstanceProcAddr(vi, "vk" #x)
    GI(CreateInstance);
    GI(EnumerateInstanceExtensionProperties);
    PFN_vkEnumerateInstanceVersion eiv = (void*)vkGetInstanceProcAddr(0, "vkEnumerateInstanceVersion");
    uint32_t iver = VK_API_VERSION_1_0;
    if (eiv) eiv(&iver);
    uint32_t nie = 0;
    vkEnumerateInstanceExtensionProperties(0, &nie, 0);
    VkExtensionProperties* ie = calloc(nie, sizeof *ie);
    vkEnumerateInstanceExtensionProperties(0, &nie, ie);
    const char* iexts[4];
    int niexts = 0;
    static const char* inames[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                                   "VK_KHR_external_semaphore_capabilities"};
    for (int i = 0; i < 3; i++)
        if (has_ext(ie, nie, inames[i])) iexts[niexts++] = inames[i];
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_spike",
                             .apiVersion = iver >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = niexts, .ppEnabledExtensionNames = iexts};
    double t0 = now_ms();
    VkResult res = vkCreateInstance(&ici, 0, &vi);
    if (res) {
        fprintf(stderr, "vkCreateInstance = %d\n", res);
        return false;
    }
    GI(EnumeratePhysicalDevices);
    GI(GetPhysicalDeviceProperties);
    GI(GetPhysicalDeviceFeatures);
    GI(GetPhysicalDeviceMemoryProperties);
    GI(GetPhysicalDeviceQueueFamilyProperties);
    GI(GetPhysicalDeviceFormatProperties);
    GI(EnumerateDeviceExtensionProperties);
    GI(CreateDevice);
    GI(GetDeviceProcAddr);
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(vi, &n, &vpd) < 0 || !n) {
        fprintf(stderr, "Vulkan: no device\n");
        return false;
    }
    vkGetPhysicalDeviceProperties(vpd, &vprops);
    vkGetPhysicalDeviceMemoryProperties(vpd, &vmem);
    VkPhysicalDeviceFeatures feat;
    vkGetPhysicalDeviceFeatures(vpd, &feat);
    fprintf(stderr, "Vulkan: %s, API %u.%u.%u (instance %u.%u), driver 0x%x\n", vprops.deviceName,
            VK_VERSION_MAJOR(vprops.apiVersion), VK_VERSION_MINOR(vprops.apiVersion),
            VK_VERSION_PATCH(vprops.apiVersion), VK_VERSION_MAJOR(iver), VK_VERSION_MINOR(iver), vprops.driverVersion);
    fprintf(stderr,
            "features: logicOp %d dualSrcBlend %d depthClamp %d fragmentStoresAndAtomics %d "
            "shaderClipDistance %d samplerAnisotropy %d independentBlend %d\n",
            feat.logicOp, feat.dualSrcBlend, feat.depthClamp, feat.fragmentStoresAndAtomics, feat.shaderClipDistance,
            feat.samplerAnisotropy, feat.independentBlend);
    fprintf(stderr,
            "limits: uboAlign %llu maxBoundSets %u maxPushConstants %u dynUBO %u "
            "timestampPeriod %.2f timestampCompute %d\n",
            (unsigned long long)vprops.limits.minUniformBufferOffsetAlignment, vprops.limits.maxBoundDescriptorSets,
            vprops.limits.maxPushConstantsSize, vprops.limits.maxDescriptorSetUniformBuffersDynamic,
            vprops.limits.timestampPeriod, vprops.limits.timestampComputeAndGraphics);
    for (uint32_t i = 0; i < vmem.memoryTypeCount; i++)
        fprintf(stderr, "memory type %u: flags 0x%x heap %u\n", i, vmem.memoryTypes[i].propertyFlags,
                vmem.memoryTypes[i].heapIndex);
    uint32_t nde = 0;
    vkEnumerateDeviceExtensionProperties(vpd, 0, &nde, 0);
    VkExtensionProperties* de = calloc(nde, sizeof *de);
    vkEnumerateDeviceExtensionProperties(vpd, 0, &nde, de);
    fprintf(stderr, "device extensions (%u):", nde);
    for (size_t i = 0; i < sizeof report_exts / sizeof *report_exts; i++)
        fprintf(stderr, " %s=%d", report_exts[i] + 3, has_ext(de, nde, report_exts[i]));
    fprintf(stderr, "\n");
    const char* dexts[32];
    int ndexts = 0;
    for (size_t i = 0; i < sizeof want_exts / sizeof *want_exts; i++)
        if (has_ext(de, nde, want_exts[i])) dexts[ndexts++] = want_exts[i];
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
                                   .pQueuePriorities = &prio};
    VkPhysicalDeviceFeatures en = {.logicOp = feat.logicOp};
    VkDeviceCreateInfo dci = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = ndexts,      .ppEnabledExtensionNames = dexts, .pEnabledFeatures = &en};
    res = vkCreateDevice(vpd, &dci, 0, &vd);
    if (res) {
        fprintf(stderr, "vkCreateDevice = %d\n", res);
        return false;
    }
    fprintf(stderr, "Vulkan: instance + device in %.1f ms\n", now_ms() - t0);
#define GD(x) vk##x = (PFN_vk##x)vkGetDeviceProcAddr(vd, "vk" #x)
    GD(GetDeviceQueue);
    GD(CreateBuffer);
    GD(GetBufferMemoryRequirements);
    GD(AllocateMemory);
    GD(BindBufferMemory);
    GD(MapMemory);
    GD(CreateImage);
    GD(GetImageMemoryRequirements);
    GD(BindImageMemory);
    GD(CreateImageView);
    GD(CreateSampler);
    GD(CreateRenderPass);
    GD(CreateFramebuffer);
    GD(CreateShaderModule);
    GD(CreatePipelineCache);
    GD(GetPipelineCacheData);
    GD(CreateGraphicsPipelines);
    GD(CreatePipelineLayout);
    GD(CreateDescriptorSetLayout);
    GD(CreateDescriptorPool);
    GD(AllocateDescriptorSets);
    GD(UpdateDescriptorSets);
    GD(CreateCommandPool);
    GD(AllocateCommandBuffers);
    GD(ResetCommandPool);
    GD(BeginCommandBuffer);
    GD(EndCommandBuffer);
    GD(CmdBeginRenderPass);
    GD(CmdEndRenderPass);
    GD(CmdBindPipeline);
    GD(CmdSetViewport);
    GD(CmdSetScissor);
    GD(CmdBindDescriptorSets);
    GD(CmdPushConstants);
    GD(CmdBindVertexBuffers);
    GD(CmdBindIndexBuffer);
    GD(CmdDrawIndexed);
    GD(CmdDraw);
    GD(CmdPipelineBarrier);
    GD(CmdCopyBufferToImage);
    GD(CmdCopyImageToBuffer);
    GD(CreateFence);
    GD(WaitForFences);
    GD(ResetFences);
    GD(QueueSubmit);
    GD(QueueWaitIdle);
    GD(DeviceWaitIdle);
    GD(GetImageSubresourceLayout);
    vkGetMemoryFdKHR = (void*)vkGetDeviceProcAddr(vd, "vkGetMemoryFdKHR");
    vkGetDeviceQueue(vd, 0, 0, &vq);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vpd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    fprintf(stderr, "D24S8 optimal attachment %d\n",
            !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
    return true;
}

static VkShaderModule vk_module(const uint32_t* code, size_t words) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * 4, .pCode = code};
    VkShaderModule m = 0;
    vkCreateShaderModule(vd, &ci, 0, &m);
    return m;
}

static const VkVertexInputBindingDescription vbind[2] = {{0, sizeof(VA), VK_VERTEX_INPUT_RATE_VERTEX},
                                                         {1, sizeof(VB), VK_VERTEX_INPUT_RATE_VERTEX}};
static const VkVertexInputAttributeDescription vattr[8] = {
    {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
    {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {2, 1, VK_FORMAT_R32G32_SFLOAT, 16},
    {3, 1, VK_FORMAT_R32G32_SFLOAT, 24},      {4, 1, VK_FORMAT_R32G32_SFLOAT, 32},
    {5, 1, VK_FORMAT_R32_SFLOAT, 40},         {7, 1, VK_FORMAT_R32G32B32_SFLOAT, 44}};

static VkPipeline vk_pipeline(VkPipelineCache cache, VkPipelineLayout layout, VkRenderPass rp, VkShaderModule vs,
                              VkShaderModule fs, int variant, bool hw) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    // the hardware vertex shaders take v0..v11 as vec4: give them one float4 buffer each
    VkVertexInputBindingDescription hb[12];
    VkVertexInputAttributeDescription ha[12];
    for (int i = 0; i < 12; i++) {
        hb[i] = (VkVertexInputBindingDescription){i, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        ha[i] = (VkVertexInputAttributeDescription){i, i, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    }
    VkPipelineVertexInputStateCreateInfo vi_ = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = hw ? 12 : 2,
        .pVertexBindingDescriptions = hw ? hb : vbind, .vertexAttributeDescriptionCount = hw ? 12 : 8,
        .pVertexAttributeDescriptions = hw ? ha : vattr};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                            .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs_ = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = variants[variant].cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        // same memory layout as GL (positive viewport), so GL's CCW is CW here
        .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = 1, .depthWriteEnable = variants[variant].depthwrite,
                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    static const VkBlendFactor bs[] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE},
                               bd[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE};
    int b = variants[variant].blend;
    // logic op COPY (GL's blend-off case) is just no blending: no logic op, which Mali
    // would run as a blend shader
    VkPipelineColorBlendAttachmentState att = {.blendEnable = b != 0,
                                               .srcColorBlendFactor = bs[b],
                                               .dstColorBlendFactor = bd[b],
                                               .colorBlendOp = VK_BLEND_OP_ADD,
                                               .srcAlphaBlendFactor = bs[b],
                                               .dstAlphaBlendFactor = bd[b],
                                               .alphaBlendOp = VK_BLEND_OP_ADD,
                                               .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &att};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 6,
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = st,
                                       .pVertexInputState = &vi_,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs_,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = layout,
                                       .renderPass = rp};
    VkPipeline p = 0;
    VkResult r = vkCreateGraphicsPipelines(vd, cache, 1, &ci, 0, &p);
    if (r) fprintf(stderr, "vkCreateGraphicsPipelines = %d\n", r);
    return p;
}

// one upload to a sampled image through a staging buffer, outside any render pass
static void vk_upload(VkCommandBuffer cb, Buf* stage, size_t* soff, Img* im, int layers, int x, int y, int w, int h,
                      const void* data, size_t bytes, bool first) {
    memcpy(stage->p + *soff, data, bytes);
    barrier(cb, im->i, VK_IMAGE_ASPECT_COLOR_BIT, layers,
            first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {.bufferOffset = *soff,
                           .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                           .imageOffset = {x, y, 0},
                           .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, stage->b, im->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, im->i, VK_IMAGE_ASPECT_COLOR_BIT, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    *soff = (*soff + bytes + 255) & ~(size_t)255;
}

static void run_vk(Result* r) {
    r->api = "Vulkan";
    if (!vk_up()) return;
    // layout: one set for everything, three dynamic uniform buffers, push constants
    VkDescriptorSetLayoutBinding lb[VKB_COUNT];
    for (int i = 0; i < VKB_COUNT; i++) {
        lb[i] = (VkDescriptorSetLayoutBinding){i,
                                               i == VKB_FREECAM  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                               : i < VKB_FREECAM ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                                 : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                               1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    VkDescriptorSetLayout dsl;
    VkDescriptorSetLayoutCreateInfo dslc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = VKB_COUNT, .pBindings = lb};
    CK(vkCreateDescriptorSetLayout(vd, &dslc, 0, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VkGlslPush)};
    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CK(vkCreatePipelineLayout(vd, &plc, 0, &layout));

    VkFormat dfmt = VK_FORMAT_D24_UNORM_S8_UINT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vpd, dfmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        dfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkAttachmentDescription ad[2] = {
        {0, VK_FORMAT_R8G8B8A8_UNORM, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {0, dfmt, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR,
         VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                          dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &cr,
                               .pDepthStencilAttachment = &dr};
    VkRenderPass rp;
    VkRenderPassCreateInfo rpc = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = ad,
                                  .subpassCount = 1, .pSubpasses = &sp};
    CK(vkCreateRenderPass(vd, &rpc, 0, &rp));

    // shaders and pipelines: cold (or from --cache), then the same again from the warm cache
    size_t cache_n = 0;
    void* cache_data = o_cache ? read_file(o_cache, &cache_n) : 0;
    VkPipelineCache cache;
    VkPipelineCacheCreateInfo pcc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = cache_data ? cache_n : 0, .pInitialData = cache_data};
    CK(vkCreatePipelineCache(vd, &pcc, 0, &cache));
    double t0 = now_ms();
    size_t nw;
    char* vsrc = vkglsl_fixup(g_drawvs, 0);
    uint32_t* vspv = vsrc ? glsl_to_spirv(vsrc, 0, &nw, &r->spirv_ms) : 0;
    if (!vspv) {
        fprintf(stderr, "vertex shader did not convert:\n%s\n", vsrc ? vsrc : "(fixup)");
        return;
    }
    VkShaderModule vsm = vk_module(vspv, nw);
    VkShaderModule* fsm = calloc(o_progs, sizeof *fsm);
    VkPipeline(*pipes)[NVARIANT] = calloc(o_progs, sizeof *pipes);
    int npipes = 0;
    for (int i = 0; i < o_progs; i++) {
        char* f = vkglsl_fixup(g_fs[i % g_nfs].src, 1);
        uint32_t* spv = f ? glsl_to_spirv(f, 1, &nw, &r->spirv_ms) : 0;
        if (!spv) {
            fprintf(stderr, "fragment shader %d did not convert\n", i);
            if (getenv("VKSPIKE_DUMP")) fprintf(stderr, "%s\n", f ? f : g_fs[i % g_nfs].src);
            return;
        }
        fsm[i] = vk_module(spv, nw);
        free(spv);
        free(f);
    }
    for (int d = 0; d < o_draws; d++) {
        Draw* w = &g_scene[d];
        if (!pipes[w->prog][w->variant]) {
            pipes[w->prog][w->variant] = vk_pipeline(cache, layout, rp, vsm, fsm[w->prog], w->variant, false);
            if (!pipes[w->prog][w->variant]) return;
            npipes++;
        }
    }
    r->build_ms = now_ms() - t0;
    fprintf(stderr, "Vulkan: %d pipelines for %d programs: %.0f ms (GLSL->SPIR-V %.0f ms of it)\n", npipes, o_progs,
            r->build_ms, r->spirv_ms);
    t0 = now_ms();
    for (int i = 0; i < o_progs; i++)
        for (int v = 0; v < NVARIANT; v++)
            if (pipes[i][v]) vk_pipeline(cache, layout, rp, vsm, fsm[i], v, false);
    fprintf(stderr, "Vulkan: the same %d pipelines again from the warm cache: %.1f ms\n", npipes, now_ms() - t0);
    // a second blend state on a known shader pair: the price of state baked into pipelines
    t0 = now_ms();
    for (int i = 0; i < o_progs && i < 10; i++)
        for (int v = 0; v < NVARIANT; v++)
            if (!pipes[i][v]) vk_pipeline(cache, layout, rp, vsm, fsm[i], v, false);
    fprintf(stderr, "Vulkan: new state variants of known shaders (up to 10 programs): %.1f ms\n", now_ms() - t0);
    if (g_npairs) {
        double sp_ms = 0;
        t0 = now_ms();
        VkShaderModule* vm = calloc(g_nvs, sizeof *vm);
        int n = g_npairs < o_progs ? g_npairs : o_progs, built = 0;
        for (int i = 0; i < n; i++) {
            int v = g_pairs[i].v;
            if (!vm[v]) {
                char* s = vkglsl_fixup(g_vs[v].src, 0);
                uint32_t* spv = s ? glsl_to_spirv(s, 0, &nw, &sp_ms) : 0;
                if (!spv) {
                    fprintf(stderr, "list vertex shader %d did not convert\n", v);
                    if (getenv("VKSPIKE_DUMP")) fprintf(stderr, "%s\n", s ? s : g_vs[v].src);
                    continue;
                }
                vm[v] = vk_module(spv, nw);
            }
            char* f = vkglsl_fixup(g_fs[g_pairs[i].f].src, 1);
            uint32_t* spv = f ? glsl_to_spirv(f, 1, &nw, &sp_ms) : 0;
            if (!spv) continue;
            if (vk_pipeline(cache, layout, rp, vm[v], vk_module(spv, nw), 0, true)) built++;
        }
        fprintf(stderr,
                "Vulkan: %d of %d list programs (hw vertex shaders) built in %.0f ms "
                "(GLSL->SPIR-V %.0f ms of it)\n",
                built, n, now_ms() - t0, sp_ms);
    }
    if (o_cache) {
        size_t sz = 0;
        vkGetPipelineCacheData(vd, cache, &sz, 0);
        void* data = malloc(sz);
        vkGetPipelineCacheData(vd, cache, &sz, data);
        FILE* f = fopen(o_cache, "wb");
        if (f) {
            fwrite(data, 1, sz, f);
            fclose(f);
        }
        fprintf(stderr, "Vulkan: pipeline cache %zu bytes written to %s\n", sz, o_cache);
    }
    if (o_compile_only) {
        r->ok = 1;
        return;
    }

    // targets, textures, samplers
    Img color, depth, tex[NTEX], lightlut, foglut, ptmap, ptnoise, proctex, shadow;
    if (!img_make(&color, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, FBW, FBH, 1,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT) ||
        !img_make(&depth, VK_IMAGE_TYPE_2D, dfmt, FBW, FBH, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        fprintf(stderr, "Vulkan: no render targets\n");
        return;
    }
    const VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int t = 0; t < NTEX; t++)
        img_make(&tex[t], VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, TEXSZ, TEXSZ, 1, su, VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&lightlut, VK_IMAGE_TYPE_1D, VK_FORMAT_R16_UNORM, 256, 1, 32, su, VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&foglut, VK_IMAGE_TYPE_1D, VK_FORMAT_R16_UNORM, 129, 1, 1, su, VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&ptmap, VK_IMAGE_TYPE_1D, VK_FORMAT_R16G16_UNORM, 128, 1, 1, su, VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&ptnoise, VK_IMAGE_TYPE_1D, VK_FORMAT_R16_UNORM, 128, 1, 1, su, VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&proctex, VK_IMAGE_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, 256, 1, 1, su, VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&shadow, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, su, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSampler srep, sclamp, smirror;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT};
    CK(vkCreateSampler(vd, &sci, 0, &srep));
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CK(vkCreateSampler(vd, &sci, 0, &sclamp));
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    CK(vkCreateSampler(vd, &sci, 0, &smirror));
    VkFramebuffer fb;
    VkImageView fbv[2] = {color.v, depth.v};
    VkFramebufferCreateInfo fbc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = rp,
                                   .attachmentCount = 2,
                                   .pAttachments = fbv,
                                   .width = FBW,
                                   .height = FBH,
                                   .layers = 1};
    CK(vkCreateFramebuffer(vd, &fbc, 0, &fb));

    // per frame in flight: command pool, fence, one ring for vertices, indices, uniforms
    VkCommandPool pool[2];
    VkCommandBuffer cmd[2];
    VkFence fence[2];
    Buf ring, stage[2], readback; // ring: both slots, slot s at s * SLOT_BYTES
    for (int s = 0; s < 2; s++) {
        VkCommandPoolCreateInfo cpc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
        CK(vkCreateCommandPool(vd, &cpc, 0, &pool[s]));
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool[s],
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        CK(vkAllocateCommandBuffers(vd, &cai, &cmd[s]));
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        CK(vkCreateFence(vd, &fci, 0, &fence[s]));
        if (!buf_make(&stage[s], 4u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            fprintf(stderr, "Vulkan: no host-visible buffers\n");
            return;
        }
    }
    if (!buf_make(&ring, 2 * SLOT_BYTES,
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        fprintf(stderr, "Vulkan: no host-visible ring\n");
        return;
    }
    buf_make(&readback, FBW * FBH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // PanVK on Bifrost (JM) turns every indexed draw into an indirect one with a GPU search
    // for the index range over the bound buffer up to ITS END: indices in a big ring cost
    // a scan of megabytes per draw. --ibuf exact gives each draw an index buffer object of
    // its exact size, aliased into the ring's memory (the top of each slot).
    size_t istride = (sizeof g_idx + 255) & ~(size_t)255;
    VkBuffer* ibufs = 0;
    if (o_ibuf_exact) {
        ibufs = calloc(2 * o_draws, sizeof *ibufs);
        for (int s = 0; s < 2; s++)
            for (int d = 0; d < o_draws; d++) {
                VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof g_idx,
                                         .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
                CK(vkCreateBuffer(vd, &ci, 0, &ibufs[s * o_draws + d]));
                VkMemoryRequirements mr;
                vkGetBufferMemoryRequirements(vd, ibufs[s * o_draws + d], &mr);
                size_t at = (s + 1) * SLOT_BYTES - (o_draws - d) * istride;
                if (at % mr.alignment) {
                    fprintf(stderr, "index buffer alignment %llu\n", (unsigned long long)mr.alignment);
                    return;
                }
                CK(vkBindBufferMemory(vd, ibufs[s * o_draws + d], ring.m, at));
            }
    }
    size_t ualign = vprops.limits.minUniformBufferOffsetAlignment;
    if (ualign < 16) ualign = 16;

    // initial contents
    {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cmd[0], &bi);
        size_t so = 0;
        for (int t = 0; t < NTEX; t++)
            vk_upload(cmd[0], &stage[0], &so, &tex[t], 1, 0, 0, TEXSZ, TEXSZ, g_texels[t], TEXSZ * TEXSZ * 4, true);
        vk_upload(cmd[0], &stage[0], &so, &lightlut, 32, 0, 0, 256, 1, g_lightlut, sizeof g_lightlut, true);
        vk_upload(cmd[0], &stage[0], &so, &foglut, 1, 0, 0, 129, 1, g_foglut, sizeof g_foglut, true);
        vk_upload(cmd[0], &stage[0], &so, &ptmap, 1, 0, 0, 128, 1, g_ptmap, sizeof g_ptmap, true);
        vk_upload(cmd[0], &stage[0], &so, &ptnoise, 1, 0, 0, 128, 1, g_ptnoise, sizeof g_ptnoise, true);
        vk_upload(cmd[0], &stage[0], &so, &proctex, 1, 0, 0, 256, 1, g_proctex, sizeof g_proctex, true);
        static uint32_t blank[64];
        vk_upload(cmd[0], &stage[0], &so, &shadow, 1, 0, 0, 8, 8, blank, sizeof blank, true);
        vkEndCommandBuffer(cmd[0]);
        vkResetFences(vd, 1, &fence[0]);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[0]};
        CK(vkQueueSubmit(vq, 1, &si, fence[0]));
        vkWaitForFences(vd, 1, &fence[0], 1, UINT64_MAX);
    }

    // descriptor sets, one per texture combination seen, kept across frames
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * 1024},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9 * 1024}};
    VkDescriptorPoolSize ps2 = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024};
    VkDescriptorPoolSize pss[3] = {ps[0], ps[1], ps2};
    VkDescriptorPool dpool;
    VkDescriptorPoolCreateInfo dpc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1024,
                                      .poolSizeCount = 3, .pPoolSizes = pss};
    CK(vkCreateDescriptorPool(vd, &dpc, 0, &dpool));
    VkDescriptorSet* sets = calloc(NTEX * NTEX * NTEX, sizeof *sets);
    int nsets = 0;
    double set_ms = 0;
    Buf ubo0;
    buf_make(&ubo0, 64 << 10, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    VkGlslPush push = {0, -1, 0};
    uint64_t f0 = 0, v0 = 0, f1, v1;
    double w0 = 0, c0 = 0, p0 = 0, rec_ms = 0, sub_ms = 0, wait_ms = 0;
    for (int frame = 0; frame < o_frames + o_warm; frame++) {
        int s = frame & 1;
        if (frame == o_warm) {
            vkDeviceWaitIdle(vd);
            gpu_engine_ns(&f0, &v0);
            w0 = now_ms();
            c0 = cpu_ms(CLOCK_THREAD_CPUTIME_ID);
            p0 = cpu_ms(CLOCK_PROCESS_CPUTIME_ID);
            rec_ms = sub_ms = wait_ms = 0;
        }
        double ta = now_ms();
        vkWaitForFences(vd, 1, &fence[s], 1, UINT64_MAX);
        vkResetFences(vd, 1, &fence[s]);
        double tb = now_ms();
        wait_ms += tb - ta;
        vkResetCommandPool(vd, pool[s], 0);
        VkCommandBuffer cb = cmd[s];
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        size_t so = 0;
        for (int k = 0; k < o_texupdates; k++) {
            int t, x, y;
            tex_update_rect(frame, k, &t, &x, &y);
            vk_upload(cb, &stage[s], &so, &tex[t], 1, x, y, 32, 32, g_upd[(frame + k) & 63], 32 * 32 * 4, false);
        }
        VkClearValue cv[2] = {{.color = {{0.1f, 0.2f, 0.3f, 1}}}, {.depthStencil = {1, 0}}};
        VkRenderPassBeginInfo rbi = {
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,     .framebuffer = fb,
            .renderArea = {{0, 0}, {FBW, FBH}},       .clearValueCount = 2, .pClearValues = cv};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp = {0, 0, FBW, FBH, 0, 1};
        vkCmdSetViewport(cb, 0, 1, &vp);
        VkRect2D sc = o_tiny ? (VkRect2D){{0, 0}, {1, 1}} : (VkRect2D){{0, 0}, {FBW, FBH}};
        vkCmdSetScissor(cb, 0, 1, &sc);
        VkPipeline curpipe = 0;
        size_t off = s * SLOT_BYTES;
        uint8_t* rp_ = ring.p;
        for (int d = 0; d < o_draws; d++) {
            Draw* w = &g_scene[d];
            size_t va = off, vb;
            if (o_nonindexed) {
                memcpy(rp_ + va, w->xa, sizeof w->xa);
                vb = va + sizeof w->xa;
                memcpy(rp_ + vb, w->xb, sizeof w->xb);
                off = vb + sizeof w->xb;
            } else {
                memcpy(rp_ + va, w->va, sizeof w->va);
                vb = va + sizeof w->va;
                memcpy(rp_ + vb, w->vb, sizeof w->vb);
                off = vb + sizeof w->vb;
            }
            size_t io = off;
            size_t ia = o_ibuf_exact ? (s + 1) * SLOT_BYTES - (o_draws - d) * istride : io;
            memcpy(rp_ + ia, g_idx, sizeof g_idx);
            size_t uo = (io + (o_ibuf_exact ? 0 : sizeof g_idx) + ualign - 1) & ~(ualign - 1);
            memcpy(rp_ + uo, &w->fu, sizeof w->fu);
            off = uo + sizeof w->fu;

            VkPipeline p = pipes[w->prog][w->variant];
            if (p != curpipe) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                if (!curpipe) vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
                curpipe = p;
            }
            int key = (w->tex[0] * NTEX + w->tex[1]) * NTEX + w->tex[2];
            if (!sets[key]) {
                double ts = now_ms();
                VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = dpool, .descriptorSetCount = 1,
                                                   .pSetLayouts = &dsl};
                CK(vkAllocateDescriptorSets(vd, &dai, &sets[key]));
                VkDescriptorBufferInfo bufi[4];
                VkDescriptorImageInfo imi[9];
                VkWriteDescriptorSet wr[VKB_COUNT];
                for (int b = 0; b < VKB_COUNT; b++) {
                    wr[b] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[key],
                                                   .dstBinding = b, .descriptorCount = 1,
                                                   .descriptorType = lb[b].descriptorType};
                    if (b <= VKB_FREECAM) {
                        bufi[b] = (VkDescriptorBufferInfo){b == VKB_FREECAM ? ubo0.b : ring.b, 0,
                                                           b == VKB_FRAGUNI   ? sizeof(FragUniforms)
                                                           : b == VKB_VERTUNI ? 1600
                                                                              : 592};
                        if (b == VKB_FREECAM) bufi[b].range = 80;
                        wr[b].pBufferInfo = &bufi[b];
                    } else {
                        Img* im[] = {&tex[w->tex[0]], &shadow, &tex[w->tex[1]], &tex[w->tex[2]], &proctex,
                                     &lightlut,       &foglut, &ptmap,          &ptnoise};
                        VkSampler smp = b <= VKB_TEX2 ? srep : b == VKB_PTNOISE ? smirror : sclamp;
                        imi[b - VKB_TEX0] =
                            (VkDescriptorImageInfo){smp, im[b - VKB_TEX0]->v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        wr[b].pImageInfo = &imi[b - VKB_TEX0];
                    }
                }
                vkUpdateDescriptorSets(vd, VKB_COUNT, wr, 0, 0);
                nsets++;
                set_ms += now_ms() - ts;
            }
            // the uniforms sit in the frame's ring: the dynamic offset picks them, so the
            // set only changes with the textures
            uint32_t dyn[3] = {0, 0, (uint32_t)uo};
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[key], 3, dyn);
            VkBuffer vbs[2] = {ring.b, ring.b};
            VkDeviceSize vos[2] = {va, vb};
            vkCmdBindVertexBuffers(cb, 0, 2, vbs, vos);
            if (o_nonindexed) vkCmdDraw(cb, NIDX, 1, 0, 0);
            else {
                if (o_ibuf_exact) vkCmdBindIndexBuffer(cb, ibufs[s * o_draws + d], 0, VK_INDEX_TYPE_UINT16);
                else vkCmdBindIndexBuffer(cb, ring.b, io, VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cb, NIDX, 1, 0, 0, 0);
            }
        }
        vkCmdEndRenderPass(cb);
        if (frame == 0 && o_shots) {
            VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {FBW, FBH, 1}};
            vkCmdCopyImageToBuffer(cb, color.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &c);
        }
        vkEndCommandBuffer(cb);
        double tc = now_ms();
        rec_ms += tc - tb;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        CK(vkQueueSubmit(vq, 1, &si, fence[s]));
        sub_ms += now_ms() - tc;
        if (frame == 0 && o_shots) {
            vkWaitForFences(vd, 1, &fence[s], 1, UINT64_MAX);
            r->shot = malloc(FBW * FBH * 4);
            memcpy(r->shot, readback.p, FBW * FBH * 4);
        }
    }
    vkDeviceWaitIdle(vd);
    r->frames = o_frames;
    r->frame_wall = (now_ms() - w0) / o_frames;
    r->frame_thread = (cpu_ms(CLOCK_THREAD_CPUTIME_ID) - c0) / o_frames;
    r->frame_proc = (cpu_ms(CLOCK_PROCESS_CPUTIME_ID) - p0) / o_frames;
    gpu_engine_ns(&f1, &v1);
    r->gpu_frag = (f1 - f0) / 1e6 / o_frames;
    r->gpu_vt = (v1 - v0) / 1e6 / o_frames;
    fprintf(stderr,
            "Vulkan: per frame record %.2f ms, submit %.3f ms, fence wait %.2f ms; %d descriptor "
            "sets written once (%.1f ms total)\n",
            rec_ms / o_frames, sub_ms / o_frames, wait_ms / o_frames, nsets, set_ms);
    list_threads("Vulkan");
    r->ok = 1;
}

// ---------------------------------------------------------------- dma-buf hand-off

// A Vulkan image exported as a dma-buf (linear, the layout every importer takes) and
// sampled by GL through EGL_EXT_image_dma_buf_import: how Vulkan frames would reach the
// present thread, which keeps its GL panels, bridge menu and filters.
static void run_dmabuf(void) {
    if (!vk_up() || !gl_up()) return;
    if (!vkGetMemoryFdKHR) {
        fprintf(stderr, "dmabuf: no vkGetMemoryFdKHR\n");
        return;
    }
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .pNext = &ext,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {FBW, FBH, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = 1,
                            .tiling = VK_IMAGE_TILING_LINEAR,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImage img;
    CK(vkCreateImage(vd, &ci, 0, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vd, img, &mr);
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &ex, .image = img};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory mem;
    CK(vkAllocateMemory(vd, &ai, 0, &mem));
    vkBindImageMemory(vd, img, mem, 0);
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(vd, img, &sub, &lay);
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    CK(vkGetMemoryFdKHR(vd, &gi, &fd));
    // fill it on the GPU: a clear by copy of a known pattern
    Buf st;
    buf_make(&st, FBW * FBH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (int i = 0; i < FBW * FBH; i++) ((uint32_t*)st.p)[i] = 0xff000000 | (i * 2654435761u >> 8);
    VkCommandPool pool;
    VkCommandPoolCreateInfo cpc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    CK(vkCreateCommandPool(vd, &cpc, 0, &pool));
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .commandBufferCount = 1};
    CK(vkAllocateCommandBuffers(vd, &cai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {FBW, FBH, 1}};
    vkCmdCopyBufferToImage(cb, st.b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);
    VkFence f;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(vd, &fci, 0, &f);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(vq, 1, &si, f));
    vkWaitForFences(vd, 1, &f, 1, UINT64_MAX);

    PFNEGLCREATEIMAGEKHRPROC ci_ = (void*)eglGetProcAddress("eglCreateImageKHR");
    void (*target)(GLenum, void*) = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    EGLint at[] = {EGL_WIDTH,
                   FBW,
                   EGL_HEIGHT,
                   FBH,
                   EGL_LINUX_DRM_FOURCC_EXT,
                   0x34324241 /* AB24 */,
                   EGL_DMA_BUF_PLANE0_FD_EXT,
                   fd,
                   EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                   (EGLint)lay.offset,
                   EGL_DMA_BUF_PLANE0_PITCH_EXT,
                   (EGLint)lay.rowPitch,
                   EGL_NONE};
    double t0 = now_ms();
    EGLImageKHR ei = ci_ ? ci_(g_egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0, at) : 0;
    if (!ei || !target) {
        fprintf(stderr, "dmabuf: EGL import failed (0x%x, target %p)\n", eglGetError(), target);
        return;
    }
    GLuint t, fbo;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    target(GL_TEXTURE_2D, ei);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    uint32_t* px = malloc(FBW * FBH * 4);
    glReadPixels(0, 0, FBW, FBH, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int bad = 0;
    for (int i = 0; i < FBW * FBH; i++) bad += px[i] != ((uint32_t*)st.p)[i];
    fprintf(stderr,
            "dmabuf: Vulkan image (pitch %llu) imported into GL in %.2f ms, %d of %d pixels "
            "differ, framebuffer status 0x%x\n",
            (unsigned long long)lay.rowPitch, now_ms() - t0, bad, FBW * FBH, glCheckFramebufferStatus(GL_FRAMEBUFFER));
}

// ---------------------------------------------------------------- main

static void save_ppm(const char* dir, const char* name, const uint8_t* px) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FBW, FBH);
    for (int y = FBH - 1; y >= 0; y--)
        for (int x = 0; x < FBW; x++) fwrite(px + (y * FBW + x) * 4, 1, 3, f);
    fclose(f);
}

static void report(Result* r) {
    if (!r->ok) {
        fprintf(stderr, "%s: did not run\n", r->api);
        return;
    }
    printf("%-6s build %7.0f ms | frame wall %6.2f ms, render thread CPU %6.2f ms, process CPU "
           "%6.2f ms = %5.1f us/draw | GPU fragment %5.2f ms, vertex-tiler %5.2f ms\n",
           r->api, r->build_ms, r->frame_wall, r->frame_thread, r->frame_proc, r->frame_proc * 1000 / o_draws,
           r->gpu_frag, r->gpu_vt);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
#define OPT(name, dst, conv)                                                                                           \
    if (!strcmp(a, name)) {                                                                                            \
        dst = conv;                                                                                                    \
        i++;                                                                                                           \
        continue;                                                                                                      \
    }
        OPT("--mode", o_mode, v);
        OPT("--list", o_list, v);
        OPT("--vert", o_vert, v);
        OPT("--draws", o_draws, atoi(v));
        OPT("--frames", o_frames, atoi(v));
        OPT("--progs", o_progs, atoi(v));
        OPT("--texupdates", o_texupdates, atoi(v));
        OPT("--cache", o_cache, v);
        OPT("--icd", o_icd, v);
        OPT("--shots", o_shots, v);
        OPT("--warm", o_warm, atoi(v));
        if (!strcmp(a, "--nonindexed")) {
            o_nonindexed = 1;
            continue;
        }
        if (!strcmp(a, "--ibuf")) {
            o_ibuf_exact = strcmp(v, "ring") != 0;
            i++;
            continue;
        }
        if (!strcmp(a, "--tiny")) {
            o_tiny = 1;
            continue;
        }
        if (!strcmp(a, "--dmabuf")) {
            o_dmabuf = 1;
            continue;
        }
        if (!strcmp(a, "--compile-only")) {
            o_compile_only = 1;
            continue;
        }
        fprintf(stderr, "unknown option %s\n", a);
        return 2;
    }
    glslang_initialize_process();
    g_drawvs = read_file(o_vert, 0);
    if (!g_drawvs) {
        fprintf(stderr, "need --vert (Tanuki's src/video/hostshaders/gpu.vert)\n");
        return 2;
    }
    if (o_list) load_list(o_list);
    if (!g_nfs) synth_fs(o_progs);
    scene_build();
    fprintf(stderr,
            "scene: %d draws a frame of %d triangles, %d programs, %d frames (+%d warm-up), "
            "%d texture updates a frame%s\n",
            o_draws, NIDX / 3, o_progs, o_frames, o_warm, o_texupdates, o_tiny ? ", 1-pixel scissor" : "");
    Result g = {.api = "GL"}, k = {.api = "Vulkan"};
    if (o_dmabuf) run_dmabuf();
    if (!strcmp(o_mode, "gl") || !strcmp(o_mode, "both")) run_gl(&g);
    if (!strcmp(o_mode, "vk") || !strcmp(o_mode, "both")) run_vk(&k);
    if (!o_compile_only) {
        if (!strcmp(o_mode, "gl") || !strcmp(o_mode, "both")) report(&g);
        if (!strcmp(o_mode, "vk") || !strcmp(o_mode, "both")) report(&k);
    }
    if (g.shot && k.shot) {
        int diff = 0, maxd = 0;
        for (int i = 0; i < FBW * FBH * 4; i++) {
            if ((i & 3) == 3) continue;
            int d = abs(g.shot[i] - k.shot[i]);
            if (d > maxd) maxd = d;
            if (d > 2) diff++;
        }
        printf("pictures: %d channel values differ by more than 2 (of %d), largest difference %d\n", diff,
               FBW * FBH * 3, maxd);
        save_ppm(o_shots, "gl", g.shot);
        save_ppm(o_shots, "vk", k.shot);
    }
    return 0;
}
