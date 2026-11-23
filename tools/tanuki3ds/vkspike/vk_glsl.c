#include "vk_glsl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* p;
    size_t n, cap;
} Str;

static void put(Str* s, const char* t, size_t n) {
    if (s->n + n + 1 > s->cap) {
        s->cap = (s->n + n + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}
static void puts_(Str* s, const char* t) { put(s, t, strlen(t)); }
static void putf(Str* s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(s, buf, n);
}

// the varyings both generators declare, in one order for both stages
static const char* varyings[] = {"color",     "texcoord0", "texcoord1", "texcoord2",
                                 "texcoordw", "normal",    "tangent",   "view"};
static const struct {
    const char* name;
    int binding;
} named[] = {
    {"VertUniforms", VKB_VERTUNI},
    {"FragConfig", VKB_FRAGCFG},
    {"FragUniforms", VKB_FRAGUNI},
    {"FreecamUniforms", VKB_FREECAM},
    {"tex0", VKB_TEX0},
    {"tex0shadow", VKB_TEX0SHADOW},
    {"tex1", VKB_TEX1},
    {"tex2", VKB_TEX2},
    {"tex3", VKB_TEX3},
    {"lightLuts", VKB_LIGHTLUTS},
    {"fogLut", VKB_FOGLUT},
    {"proctexMapLut", VKB_PTMAP},
    {"proctexNoiseLut", VKB_PTNOISE},
};

static int binding_of(const char* name, size_t n) {
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if (strlen(named[i].name) == n && !memcmp(named[i].name, name, n)) return named[i].binding;
    return -1;
}
static int varying_of(const char* name, size_t n) {
    for (size_t i = 0; i < sizeof varyings / sizeof varyings[0]; i++)
        if (strlen(varyings[i]) == n && !memcmp(varyings[i], name, n)) return (int)i;
    return -1;
}

// the last identifier before ';' or '{' on a declaration line
static const char* last_ident(const char* b, const char* e, size_t* n) {
    while (e > b && (e[-1] == ';' || e[-1] == '{' || isspace((unsigned char)e[-1]))) e--;
    const char* s = e;
    while (s > b && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) s--;
    *n = e - s;
    return s;
}
static const char* skip_ws(const char* p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    return p;
}
static int starts(const char* p, const char* e, const char* w) {
    size_t n = strlen(w);
    return (size_t)(e - p) >= n && !memcmp(p, w, n);
}

char* vkglsl_fixup(const char* src, int stage) {
    Str o = {};
    int depth = 0, pushed = 0, ok = 1;
    const char* p = src;
    while (*p) {
        const char* e = strchr(p, '\n');
        const char* next = e ? e + 1 : p + strlen(p);
        if (!e) e = next;
        const char* t = skip_ws(p, e);
        int handled = 0;
        if (depth == 0) {
            if (starts(t, e, "#version")) {
                puts_(&o, "#version 450\n");
                handled = 1;
            } else if ((stage == 0 && starts(t, e, "out ")) || (stage == 1 && starts(t, e, "in "))) {
                size_t n;
                const char* id = last_ident(t, e, &n);
                int loc = varying_of(id, n);
                if (loc < 0) ok = 0;
                putf(&o, "layout(location=%d) ", loc);
                put(&o, t, e - t);
                puts_(&o, "\n");
                handled = 1;
            } else if (stage == 1 && starts(t, e, "out ")) {
                puts_(&o, "layout(location=0) ");
                put(&o, t, e - t);
                puts_(&o, "\n");
                handled = 1;
            } else if (starts(t, e, "layout (std140) uniform") || starts(t, e, "layout(std140) uniform")) {
                const char* u = strstr(t, "uniform") + 7;
                u = skip_ws(u, e);
                const char* ue = u;
                while (ue < e && (isalnum((unsigned char)*ue) || *ue == '_')) ue++;
                int b = binding_of(u, ue - u);
                if (b < 0) ok = 0;
                putf(&o, "layout(std140, set=0, binding=%d) uniform ", b);
                put(&o, u, e - u);
                puts_(&o, "\n");
                handled = 1;
            } else if (starts(t, e, "uniform ")) {
                size_t n;
                const char* id = last_ident(t, e, &n);
                const char* ty = skip_ws(t + 8, e);
                if (starts(ty, e, "sampler")) {
                    int b = binding_of(id, n);
                    if (b < 0) ok = 0;
                    putf(&o, "layout(set=0, binding=%d) ", b);
                    put(&o, t, e - t);
                    puts_(&o, "\n");
                } else {
                    // depthOffset, depthScale, depthWBuffer: one push constant block,
                    // which must match VkGlslPush
                    if (!pushed) {
                        puts_(&o, "layout(push_constant) uniform PushC { float depthOffset; float "
                                  "depthScale; bool depthWBuffer; };\n");
                        pushed = 1;
                    }
                    if (!(n == 11 && !memcmp(id, "depthOffset", 11)) && !(n == 10 && !memcmp(id, "depthScale", 10)) &&
                        !(n == 12 && !memcmp(id, "depthWBuffer", 12)))
                        ok = 0;
                }
                handled = 1;
            } else if (stage == 0 && starts(t, e, "void main()")) {
                puts_(&o, "void main_gl()");
                put(&o, t + 11, e - (t + 11));
                puts_(&o, "\n");
                handled = 1;
            }
        }
        if (!handled) {
            put(&o, p, e - p);
            puts_(&o, "\n");
        }
        for (const char* c = p; c < e; c++) {
            if (*c == '{') depth++;
            if (*c == '}') depth--;
        }
        p = next;
    }
    if (stage == 0) {
        // GL's clip depth is -1..1 (the generators map to it), Vulkan's 0..1. Memory
        // layout of the images stays GL's: viewport and scissor as in GL, front face CW.
        puts_(&o, "void main() {\n    main_gl();\n    gl_Position.z = (gl_Position.z + gl_Position.w) * "
                  "0.5;\n}\n");
    }
    if (!ok) {
        free(o.p);
        return 0;
    }
    return o.p;
}
