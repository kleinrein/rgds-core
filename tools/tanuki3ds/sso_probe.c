// sso_probe LIST [PAIRS]: how long a new program takes on the handheld's GPU when the
// vertex and fragment shaders are linked together (what the engine does today) against
// separate programs (ARB_separate_shader_objects), where a new fragment shader links on
// its own and an already built vertex program is reused. Each build is timed through its
// first draw, since a driver may finish the work there. Reads a game's shaders/<rom>.list.
// Run with MESA_SHADER_CACHE_DISABLE=true for cold numbers (sso-probe.sh on the handheld).
//
// Build in the VM (from a Tanuki tree with build/release objects):
//   clang-19 -O2 -I src -I src/glad -o sso_probe sso_probe.c build/release/glad/glad.c.o -lEGL
#include <EGL/egl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "glad/glad.h"

// glad here is GL 3.3 core only: the separate-program entry points are loaded by hand
static void (*pGenProgramPipelines)(GLsizei, GLuint*);
static void (*pBindProgramPipeline)(GLuint);
static void (*pUseProgramStages)(GLuint, GLbitfield, GLuint);
static void (*pProgramParameteri)(GLuint, GLenum, GLint);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

typedef struct {
    unsigned long long hash;
    char* src;
} Src;
static Src vs[4096], fs[16384];
static int nvs, nfs;
static struct {
    int v, f;
} pairs[32768];
static int npairs;

static int find(Src* t, int n, unsigned long long h) {
    for (int i = 0; i < n; i++)
        if (t[i].hash == h) return i;
    return -1;
}

static bool load(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    unsigned head[2];
    if (fread(head, sizeof head, 1, fp) != 1 || head[0] != 0x314c5054) return false;
    for (int type; (type = fgetc(fp)) != EOF;) {
        if (type == 'V' || type == 'F') {
            unsigned long long h;
            unsigned len;
            if (fread(&h, 8, 1, fp) != 1 || fread(&len, 4, 1, fp) != 1 || len > 1 << 22) break;
            char* s = malloc(len + 1);
            if (fread(s, 1, len, fp) != len) break;
            s[len] = 0;
            if (type == 'V' && nvs < 4096) vs[nvs++] = (Src) {h, s};
            if (type == 'F' && nfs < 16384) fs[nfs++] = (Src) {h, s};
        } else if (type == 'P') {
            unsigned long long h[2];
            if (fread(h, 16, 1, fp) != 1) break;
            int v = find(vs, nvs, h[0]), f = find(fs, nfs, h[1]);
            if (v >= 0 && f >= 0 && npairs < 32768) pairs[npairs].v = v, pairs[npairs++].f = f;
        } else {
            break;
        }
    }
    fclose(fp);
    return true;
}

static GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "compile: %s\n", log);
    }
    return s;
}

static GLuint link(GLuint a, GLuint b, bool separable) {
    GLuint p = glCreateProgram();
    if (separable) pProgramParameteri(p, GL_PROGRAM_SEPARABLE, GL_TRUE);
    glAttachShader(p, a);
    if (b) glAttachShader(p, b);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link: %s\n", log);
    }
    // the engine's bindings (renderer_gl.c link_program)
    const char* blocks[] = {"VertUniforms", "FragConfig", "FragUniforms", "FreecamUniforms"};
    for (int i = 0; i < 4; i++) {
        GLuint idx = glGetUniformBlockIndex(p, blocks[i]);
        if (idx != GL_INVALID_INDEX) glUniformBlockBinding(p, idx, i);
    }
    return p;
}

static unsigned char pix_a[4096][64 * 64 * 4];
static void draw(void) {
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
}
static void grab(unsigned char* out) {
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, out);
}
static void clear(void) {
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || !load(argv[1])) return puts("usage: sso_probe LIST [PAIRS]"), 1;
    int want = argc > 2 ? atoi(argv[2]) : 60;

    EGLDisplay d = eglGetPlatformDisplay(0x31DD, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(d, NULL, NULL)) return puts("no egl"), 1;
    eglBindAPI(EGL_OPENGL_API);
    EGLint xa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                   EGL_NONE};
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint ncfg = 0;
    eglChooseConfig(d, ca, &cfg, 1, &ncfg);
    EGLContext ctx = eglCreateContext(d, ncfg ? cfg : NULL, EGL_NO_CONTEXT, xa);
    if (ctx == EGL_NO_CONTEXT) return puts("no context"), 1;
    if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return puts("no ctx"), 1;
    gladLoadGLLoader((GLADloadproc) eglGetProcAddress);
    printf("%s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    pProgramParameteri = (void*) eglGetProcAddress("glProgramParameteri");
    bool has_sso = false;
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    for (int i = 0; i < next; i++)
        has_sso |= !strcmp((const char*) glGetStringi(GL_EXTENSIONS, i), "GL_ARB_separate_shader_objects");
    printf("GL_ARB_separate_shader_objects: %s\n", has_sso ? "yes" : "no");
    printf("list: %d vertex, %d fragment sources, %d pairs\n", nvs, nfs, npairs);

    GLuint fbt, fb, vao, ubo;
    glGenTextures(1, &fbt);
    glBindTexture(GL_TEXTURE_2D, fbt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbt, 0);
    glViewport(0, 0, 64, 64);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    {
        static float junk[16384];
        unsigned r = 12345;
        for (int i = 0; i < 16384; i++) {
            r = r * 1103515245 + 12345;
            junk[i] = ((r >> 8) & 0xffff) / 65535.0f;
        }
        glBufferData(GL_UNIFORM_BUFFER, sizeof junk, junk, GL_STATIC_DRAW);
    }
    for (int i = 0; i < 4; i++) glBindBufferBase(GL_UNIFORM_BUFFER, i, ubo);

    // pairs in list order, each fragment text once (as text sharing does), at most `want`
    int pick[4096], n = 0;
    for (int i = 0; i < npairs && n < want; i++) {
        bool seen = false;
        for (int j = 0; j < n; j++)
            seen |= !strcmp(fs[pairs[pick[j]].f].src, fs[pairs[i].f].src);
        if (!seen) pick[n++] = i;
    }

    // A: together, as the engine links today (vertex shader compiled again per pair)
    double a_total = 0, a_max = 0;
    for (int i = 0; i < n; i++) {
        double t0 = now_ms();
        GLuint v = shader(GL_VERTEX_SHADER, vs[pairs[pick[i]].v].src);
        GLuint f = shader(GL_FRAGMENT_SHADER, fs[pairs[pick[i]].f].src);
        GLuint p = link(v, f, false);
        glUseProgram(p);
        clear();
        draw();
        double t = now_ms() - t0;
        grab(pix_a[i]);
        a_total += t;
        if (t > a_max) a_max = t;
        glUseProgram(0);
        glDeleteProgram(p);
        glDeleteShader(v);
        glDeleteShader(f);
    }
    printf("together: %d programs, %.1f ms each, worst %.1f ms\n", n, a_total / n, a_max);
    if (!has_sso) return 0;

    // B: separate programs; a vertex program is built once and kept
    GLuint vprog[4096] = {0};
    GLuint pipe;
    pGenProgramPipelines = (void*) eglGetProcAddress("glGenProgramPipelines");
    pBindProgramPipeline = (void*) eglGetProcAddress("glBindProgramPipeline");
    pUseProgramStages = (void*) eglGetProcAddress("glUseProgramStages");
    if (!pGenProgramPipelines || !pBindProgramPipeline || !pUseProgramStages || !pProgramParameteri)
        return puts("separate-program entry points missing"), 1;
    pGenProgramPipelines(1, &pipe);
    pBindProgramPipeline(pipe);
    double b_total = 0, b_max = 0, v_total = 0;
    int reported = 0, mism = 0, worst = 0, drawn = 0;
    int vbuilt = 0;
    for (int i = 0; i < n; i++) {
        int vi = pairs[pick[i]].v;
        double t0 = now_ms();
        if (!vprog[vi]) {
            GLuint v = shader(GL_VERTEX_SHADER, vs[vi].src);
            vprog[vi] = link(v, 0, true);
            glDeleteShader(v);
            v_total += now_ms() - t0;
            vbuilt++;
        }
        double t1 = now_ms();
        GLuint f = shader(GL_FRAGMENT_SHADER, fs[pairs[pick[i]].f].src);
        GLuint fp = link(f, 0, true);
        pUseProgramStages(pipe, GL_VERTEX_SHADER_BIT, vprog[vi]);
        pUseProgramStages(pipe, GL_FRAGMENT_SHADER_BIT, fp);
        GLenum e0 = glGetError();
        clear();
        draw();
        double t = now_ms() - t1;
        GLenum e1 = glGetError();
        if ((e0 || e1) && !reported++) {
            void (*validate)(GLuint) = (void*) eglGetProcAddress("glValidateProgramPipeline");
            void (*plog)(GLuint, GLsizei, GLsizei*, char*) =
                (void*) eglGetProcAddress("glGetProgramPipelineInfoLog");
            char log[1024] = "";
            if (validate && plog) {
                validate(pipe);
                plog(pipe, sizeof log, NULL, log);
            }
            printf("pair %d: error before draw 0x%x, after 0x%x; pipeline log: %s\n", i, e0, e1, log);
        }
        static unsigned char pix_b[64 * 64 * 4];
        grab(pix_b);
        int diff = 0, maxd = 0, nonbg = 0;
        for (int k = 0; k < 64 * 64 * 4; k++) {
            int d = abs(pix_a[i][k] - pix_b[k]);
            diff += d > 2;
            if (d > maxd) maxd = d;
        }
        for (int k = 0; k < 64 * 64; k++)
            nonbg += pix_b[k * 4] != 64 || pix_b[k * 4 + 1] != 128 || pix_b[k * 4 + 2] != 191;
        if (diff) mism++;
        if (maxd > worst) worst = maxd;
        if (nonbg) drawn++;
        b_total += t;
        if (t > b_max) b_max = t;
        pUseProgramStages(pipe, GL_FRAGMENT_SHADER_BIT, 0);
        glDeleteProgram(fp);
        glDeleteShader(f);
    }
    printf("separate: %d vertex programs %.1f ms each; fragment %.1f ms each, worst %.1f ms\n",
           vbuilt, vbuilt ? v_total / vbuilt : 0, b_total / n, b_max);
    printf("pictures: %d of %d pairs drew anything; %d differ from the together build (worst %d/255)\n",
           drawn, n, mism, worst);
    // a second draw with known stages: does the driver still build anything per pair?
    double c0 = now_ms();
    for (int i = 0; i < n && i < 20; i++) draw();
    printf("plain draws after: %.2f ms each\n", (now_ms() - c0) / (n < 20 ? n : 20));
    GLenum err = glGetError();
    if (err) printf("gl error 0x%x\n", err);
    return 0;
}
