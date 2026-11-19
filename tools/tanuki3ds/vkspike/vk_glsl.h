// Turns the GLSL 330 that Tanuki3DS generates (shaderdec.c, shadergen_fs.c, hostshaders/)
// into GLSL 450 for Vulkan without touching the generators: explicit locations for the
// varyings, set/binding for samplers and uniform blocks, the loose depth uniforms as push
// constants, and GL's -1..1 clip depth folded back to Vulkan's 0..1.
#ifndef VK_GLSL_H
#define VK_GLSL_H

// bindings in descriptor set 0, shared by every pipeline (one layout for all)
enum {
    VKB_VERTUNI = 0,  // VertUniforms (hw vertex shaders), dynamic offset
    VKB_FRAGCFG = 1,  // FragConfig (uber shader), dynamic offset
    VKB_FRAGUNI = 2,  // FragUniforms, dynamic offset
    VKB_FREECAM = 3,  // FreecamUniforms
    VKB_TEX0 = 4,
    VKB_TEX0SHADOW = 5,
    VKB_TEX1 = 6,
    VKB_TEX2 = 7,
    VKB_TEX3 = 8,
    VKB_LIGHTLUTS = 9,
    VKB_FOGLUT = 10,
    VKB_PTMAP = 11,
    VKB_PTNOISE = 12,
    VKB_COUNT = 13,
};

// push constants: what the GL path sets with glUniform per program
typedef struct {
    float depthOffset;
    float depthScale;
    int depthWBuffer;
} VkGlslPush;

// stage 0 = vertex, 1 = fragment. Returns a malloc'd string, or 0 for an unknown
// declaration it cannot place (the caller then keeps the draw on GL).
char* vkglsl_fixup(const char* src, int stage);

#endif
