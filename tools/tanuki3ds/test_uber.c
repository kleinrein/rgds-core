// Renders random fragment configurations with the generated shader and with the
// uber shader and compares the pixels. Software GL through EGL surfaceless.
#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "video/shadergen_fs.h"

bool g_infologs;
#define W 48
#define H 48
#define CW 400
#define CH 240

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static unsigned long long rs = 88172645463325252ull;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned) (rs >> 16);
}
static float frnd(void) { return (rnd() & 0xffff) / 65535.0f; }
static int pick(int n) { return rnd() % n; }

static const char* vsrc =
    "#version 330 core\n"
    "layout(location=0) in vec2 a;\n"
    "out vec4 color; out vec2 texcoord0; out vec2 texcoord1; out vec2 texcoord2;\n"
    "out float texcoordw; out vec3 normal; out vec3 tangent; out vec3 view;\n"
    "void main(){ vec2 u=a*0.5+0.5; gl_Position=vec4(a,a.x*0.6-0.1,1);\n"
    "color=vec4(u,1-u.x,0.3+0.7*u.y); texcoord0=u*1.7-0.3; texcoord1=u.yx*1.3;\n"
    "texcoord2=u*0.8+0.1; texcoordw=0.5+u.x;\n"
    "normal=normalize(vec3(a*0.6,1)); tangent=normalize(vec3(1,a.y*0.4,-a.x*0.3));\n"
    "view=vec3(-a*0.8,1.5);}\n";

static GLuint compile(GLenum type, const char* src, int* ok) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint r;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &r);
    if (!r) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "compile failed: %s\n", log);
    }
    *ok = r;
    return sh;
}

static GLuint link(GLuint vs, const char* fsrc, int uber) {
    int ok;
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc, &ok);
    if (!ok) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint r;
    glGetProgramiv(p, GL_LINK_STATUS, &r);
    if (!r) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link failed: %s\n", log);
        return 0;
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex0"), 0);
    glUniform1i(glGetUniformLocation(p, "tex0shadow"), 0);
    glUniform1i(glGetUniformLocation(p, "tex1"), 1);
    glUniform1i(glGetUniformLocation(p, "tex2"), 2);
    glUniform1i(glGetUniformLocation(p, "tex3"), 3);
    glUniform1i(glGetUniformLocation(p, "lightLuts"), 4);
    glUniform1i(glGetUniformLocation(p, "fogLut"), 5);
    glUniform1i(glGetUniformLocation(p, "proctexMapLut"), 6);
    glUniform1i(glGetUniformLocation(p, "proctexNoiseLut"), 7);
    if (uber) glUniformBlockBinding(p, glGetUniformBlockIndex(p, "FragConfig"), 1);
    GLuint bi = glGetUniformBlockIndex(p, "FragUniforms");
    if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 2);
    glDeleteShader(fs);
    return p;
}

static void randcfg(FragConfig* c) {
    static const int srcs[] = {0, 1, 2, 3, 4, 5, 6, 13, 14, 15, 15, 15, 9};
    static const float scales[] = {1, 1, 1, 2, 4};
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 6; i++) {
        int plain = pick(3) == 0;
        typeof(&c->tev[0].rgb) s[2] = {&c->tev[i].rgb, &c->tev[i].a};
        for (int k = 0; k < 2; k++) {
            if (plain) {
                s[k]->src0 = 15;
                s[k]->scale = 1;
                continue;
            }
            s[k]->src0 = srcs[pick(13)];
            s[k]->src1 = srcs[pick(13)];
            s[k]->src2 = srcs[pick(13)];
            s[k]->op0 = pick(k ? 9 : 15);
            s[k]->op1 = pick(k ? 9 : 15);
            s[k]->op2 = pick(k ? 9 : 15);
            s[k]->combiner = pick(11);
            s[k]->scale = scales[pick(5)];
        }
    }
    c->tev_buffer.fogmode = pick(2) ? 5 : pick(5);
    c->tev_buffer.update_rgb = pick(16);
    c->tev_buffer.update_alpha = pick(16);
    c->tev_buffer.zflip = pick(2);
    c->texconfig.tex0enable = pick(4) != 0;
    c->texconfig.tex1enable = pick(2);
    c->texconfig.tex2enable = pick(2);
    c->texconfig.tex3enable = pick(3) == 0;
    c->texconfig.tex3coord = pick(3);
    c->texconfig.tex2coord = pick(2);
    c->tex0type = pick(6) == 0 ? (pick(2) ? 3 : 1) : 0;
    c->shadowPerspective = pick(2);
    c->proctex.clampU = pick(4);
    c->proctex.clampV = pick(4);
    c->proctex.rgbCombiner = pick(11);
    c->proctex.alphaCombiner = pick(11);
    c->proctex.separateAlpha = pick(2);
    c->proctex.noise = pick(2);
    c->lightDisable = pick(3) == 0;
    c->numlights = 1 + pick(4); // none leaves the fresnel input undefined in the generated shader
    int perm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 7; i > 0; i--) {
        int j = pick(i + 1), t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (int i = 0; i < 8; i++) {
        c->lightPerm |= perm[i] << 4 * i;
        c->light[i].config.w = pick(16);
    }
    c->lconfig0.shadow = pick(3) == 0;
    if (c->lconfig0.shadow) {
        c->lconfig0.shadowPrimary = pick(2);
        c->lconfig0.shadowSecondary = pick(2);
        c->lconfig0.shadowInv = pick(2);
        c->lconfig0.shadowAlpha = pick(2);
    }
    c->lconfig0.frPrimary = pick(2);
    c->lconfig0.frSecondary = pick(2);
    c->lconfig0.lightenv = pick(8);
    c->lconfig0.bumpTex = pick(4);
    c->lconfig0.shadowTex = pick(4);
    c->lconfig0.clampHighlights = pick(2);
    c->lconfig0.bumpMode = pick(3);
    c->lconfig0.noRecalcBumpVec = pick(2);
    c->lconfig1.w = rnd() ^ rnd() << 16;
    c->llutAbs = rnd() ^ rnd() << 16;
    c->llutSel = rnd() ^ rnd() << 16;
    c->llutScale = rnd() ^ rnd() << 16;
    c->alphatest = pick(3) == 0;
    c->alphafunc = pick(8);
}

static void randuni(FragUniforms* u) {
    float* f = (float*) u;
    for (size_t i = 0; i < sizeof *u / 4; i++) f[i] = frnd();
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 3; k++) {
            u->light[i].vec[k] = frnd() * 4 - 2;
            u->light[i].spotdir[k] = frnd() * 2 - 1;
        }
        u->light[i].attn_bias = frnd() * 0.3f;
        u->light[i].attn_scale = frnd() * 0.2f;
    }
    u->ptNoiseU.freq = 0.5f + frnd();
    u->ptNoiseV.freq = 0.5f + frnd();
    u->shadowBias = 0.01f;
}

static GLuint tex(GLenum target, int unit, GLenum ifmt, GLenum fmt, int w, int h, int comps) {
    GLuint t;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(target, t);
    float* d = malloc(sizeof(float) * w * (h ? h : 1) * comps);
    for (int i = 0; i < w * (h ? h : 1) * comps; i++) d[i] = frnd();
    if (target == GL_TEXTURE_1D) glTexImage1D(target, 0, ifmt, w, 0, fmt, GL_FLOAT, d);
    else glTexImage2D(target, 0, ifmt, w, h, 0, fmt, GL_FLOAT, d);
    free(d);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}

int main(int argc, char** argv) {
    int count = argc > 1 ? atoi(argv[1]) : 300;
    EGLDisplay d = eglGetPlatformDisplay(0x31DD, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(d, NULL, NULL)) return puts("no egl"), 1;
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n;
    eglChooseConfig(d, ca, &cfg, 1, &n);
    EGLint xa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                   EGL_NONE};
    EGLContext ctx = eglCreateContext(d, n ? cfg : NULL, EGL_NO_CONTEXT, xa);
    if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return puts("no ctx"), 1;
    gladLoadGLLoader((GLADloadproc) eglGetProcAddress);
    printf("%s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint fbt, fb;
    glGenTextures(1, &fbt);
    glBindTexture(GL_TEXTURE_2D, fbt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbt, 0);
    glViewport(0, 0, W, H);

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, 0);

    tex(GL_TEXTURE_2D, 0, GL_RGBA8, GL_RGBA, 16, 16, 4);
    tex(GL_TEXTURE_2D, 1, GL_RGBA8, GL_RGBA, 16, 16, 4);
    tex(GL_TEXTURE_2D, 2, GL_RGBA8, GL_RGBA, 16, 16, 4);
    tex(GL_TEXTURE_1D, 3, GL_RGBA8, GL_RGBA, 32, 0, 4);
    tex(GL_TEXTURE_1D_ARRAY, 4, GL_R32F, GL_RED, 256, 24, 1);
    tex(GL_TEXTURE_1D, 5, GL_R32F, GL_RED, 128, 0, 1);
    tex(GL_TEXTURE_1D, 6, GL_RG32F, GL_RG, 128, 0, 2);
    tex(GL_TEXTURE_1D, 7, GL_R32F, GL_RED, 128, 0, 1);

    GLuint ubo[2];
    glGenBuffers(2, ubo);

    int ok;
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc, &ok);
    char* usrc = shader_gen_uber_fs();
    double t0 = now_ms();
    GLuint uber = link(vs, usrc, 1);
    if (!uber) return puts("uber shader failed"), 1;
    glFinish();
    printf("uber program link %.0f ms\n", now_ms() - t0);
    double exact_ms = 0, draw_ms[2] = {0, 0};

    bool cost = getenv("TEST_UBER_COST") != NULL;
    GLuint cfbt, cfb;
    glGenTextures(1, &cfbt);
    glActiveTexture(GL_TEXTURE0 + 9);
    glBindTexture(GL_TEXTURE_2D, cfbt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, CW, CH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &cfb);
    glBindFramebuffer(GL_FRAMEBUFFER, cfb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cfbt, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    double cost_ms[2] = {0, 0};
    int bad = 0, skipped = 0, worstn = 0;
    static unsigned char a[W * H * 4], b[W * H * 4];
    for (int t = 0; t < count; t++) {
        FragConfig c;
        FragUniforms u;
        randcfg(&c);
        randuni(&u);
        char* src = shader_gen_fs(&c);
        t0 = now_ms();
        GLuint exact = link(vs, src, 0);
        exact_ms += now_ms() - t0;
        if (!exact) {
            skipped++;
            free(src);
            continue;
        }
        unsigned char cbuf[FRAGCONFIG_BLOCK_SIZE] = {0};
        memcpy(cbuf, &c, sizeof c);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]);
        glBufferData(GL_UNIFORM_BUFFER, sizeof cbuf, cbuf, GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo[0]);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]);
        glBufferData(GL_UNIFORM_BUFFER, sizeof u, &u, GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, ubo[1]);

        GLuint progs[2] = {exact, uber};
        unsigned char* out[2] = {a, b};
        for (int k = 0; k < 2; k++) {
            glClearColor(1, 0, 1, 0.5f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(progs[k]);
            t0 = now_ms();
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out[k]);
            draw_ms[k] += now_ms() - t0;
        }
        if (cost) {
            glBindFramebuffer(GL_FRAMEBUFFER, cfb);
            glViewport(0, 0, CW, CH);
            for (int k = 0; k < 2; k++) {
                glUseProgram(progs[k]);
                glClear(GL_COLOR_BUFFER_BIT);
                glFinish();
                t0 = now_ms();
                for (int r = 0; r < 20; r++) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glFinish();
                cost_ms[k] += (now_ms() - t0) / 20;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glViewport(0, 0, W, H);
        }
        int diffpx = 0, maxd = 0;
        for (int i = 0; i < W * H; i++) {
            int m = 0;
            for (int k = 0; k < 4; k++) {
                int dd = abs(a[i * 4 + k] - b[i * 4 + k]);
                if (dd > m) m = dd;
            }
            if (m > maxd) maxd = m;
            if (m > 2) diffpx++;
        }
        if (diffpx > worstn) worstn = diffpx;
        if (diffpx > W * H / 100) {
            bad++;
            if (bad <= 8) {
                printf("MISMATCH #%d: %d px differ, max %d; lights %d disable %d fog %d "
                       "alphatest %d/%d tex %x t0type %d lenv %d\n",
                       t, diffpx, maxd, c.numlights, c.lightDisable, c.tev_buffer.fogmode,
                       c.alphatest, c.alphafunc, c.texconfig.w, c.tex0type,
                       c.lconfig0.lightenv);
                char name[64];
                snprintf(name, sizeof name, "/tmp/mismatch-%d.frag", t);
                FILE* f = fopen(name, "w");
                fputs(src, f);
                fclose(f);
                snprintf(name, sizeof name, "/tmp/mismatch-%d.cfg", t);
                f = fopen(name, "wb");
                fwrite(&c, sizeof c, 1, f);
                fclose(f);
            }
        }
        glDeleteProgram(exact);
        free(src);
    }
    printf("configs %d, skipped (exact shader did not compile) %d, mismatches %d, "
           "worst differing pixels %d of %d\n",
           count, skipped, bad, worstn, W * H);
    printf("exact link %.0f ms each; draw+read exact %.2f ms, uber %.2f ms\n",
           exact_ms / count, draw_ms[0] / count, draw_ms[1] / count);
    if (cost) printf("one 400x240 layer: exact %.2f ms, uber %.2f ms\n", cost_ms[0] / count, cost_ms[1] / count);
    return bad != 0;
}
