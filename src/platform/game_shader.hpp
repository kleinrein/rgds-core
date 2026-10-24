#pragma once
#include "platform/emulator_overlay.hpp"
#include "platform/hd2d_shader.hpp"
#include "platform/shader_catalog.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace rgds {
// Apply effects to the native DS texture during the existing screen draw.
// Sampling a scaled framebuffer with invented 256x192 uniforms cannot implement
// sharp bilinear correctly and adds an unnecessary full-screen copy every frame.
class GameShader {
    using U = unsigned int;
    using I = int;
    using B = unsigned char;
    using P = std::ptrdiff_t;
    struct Program {
        U id = 0;
        I image = -1, size = -1, target = -1, source = -1, swap = -1, modulation = -1;
        // HD sampling inside the program (hd2d_sample), for presets that read the picture once.
        I hd_src = -1, hd_rect = -1, hd_px = -1, hd_swap = -1, hd_mask = -1, hd_rules = -1, hd_out = -1;
        I hd_edge = -1, hd_smooth = -1, hd_soft = -1;
    };
    // HD 2D (hd2d_shader.hpp): the rules and reconstruction passes and their render targets, per GL context.
    struct Hd {
        U rules = 0, reconstruct = 0, quad = 0, rules_texture = 0, rules_target = 0, image = 0, image_target = 0;
        int width = 0, height = 0;
        bool failed = false;
        bool said_drawing = false, said_failed = false; // one log line each
    };
    struct Gpu {
        std::array<void*, 64> functions{};
        std::map<std::array<int, 5>, Program> programs;
        Hd hd;
        U buffer = 0;
        bool high_precision = false, precision_known = false, vertices_known = false;
        std::array<float, 16> vertices{};
    };
    std::mutex mutex;
    std::map<void*, Gpu> contexts;
    // HD: the rules texture DSCore computed on the CPU for each screen texture (rgds_hd_rules).
    std::mutex pairs_mutex;
    std::map<SDL_Texture*, SDL_Texture*> rules_for;
    // Presets that read the picture once can sample the reconstruction directly; the rest read it from an image.
    bool reads_once(int selected) {
        const auto& fragment = source(selected).second;
        size_t reads = 0;
        for (size_t at = 0; (at = fragment.find("texture2D(u_texture", at)) != std::string::npos; ++at) ++reads;
        return reads <= 1;
    }
    std::map<int, std::pair<std::string, std::string>> sources;
    static std::string section(const std::string& text, const std::string& tag) {
        auto begin = text.find('<' + tag + '>');
        if (begin == std::string::npos) return {};
        begin += tag.size() + 2;
        auto end = text.find("</" + tag + '>', begin);
        return end == std::string::npos ? std::string{} : text.substr(begin, end - begin);
    }
    const std::pair<std::string, std::string>& source(int selected) {
        if (selected == 0) { // HD with no filter: the reconstruction shown as it is
            static const std::pair<std::string, std::string> copy{
                "attribute vec2 a_vertex_coordinate;attribute vec2 a_texture_coordinate;varying vec2 "
                "v_texture_coordinate;\nvoid main(){v_texture_coordinate=a_texture_coordinate;gl_Position=vec4(a_"
                "vertex_coordinate,0.0,1.0);}\n",
                "uniform sampler2D u_texture;\nuniform vec4 u_texture_size;\nvarying vec2 v_texture_coordinate;\nvoid "
                "main(){gl_FragColor=texture2D(u_texture,v_texture_coordinate);}\n"};
            return copy;
        }
        auto found = sources.find(selected);
        if (found != sources.end()) return found->second;
        std::pair<std::string, std::string> value;
        const char* folder = std::getenv("RGDS_SHADER_DIR");
        if (folder && selected > 0 && selected < shader_count) {
            std::ifstream file(std::string(folder) + '/' + shader_files[selected] + ".dsd");
            std::string text(std::istreambuf_iterator<char>(file), {});
            value = {section(text, "vertex"), section(text, "fragment")};
            if (value.first.empty() || value.second.empty())
                std::fprintf(stderr, "RGDS shader: missing or invalid preset %s\n", shader_files[selected]);
        }
        return sources.emplace(selected, std::move(value)).first->second;
    }
    // Original DraStic .dsd files depend on the engine's precision preamble.
    // GLES fragment shaders have no default float precision. Give BOTH stages
    // the same precision so their varyings and shared uniforms also link.
    static std::string prepare(const std::string& text, bool high_precision) {
        std::string prefix = std::string("\n#ifdef GL_ES\nprecision ") + (high_precision ? "highp" : "mediump") +
                             " float;\nprecision mediump int;\n#endif\n";
        auto version = text.find("#version");
        if (version != std::string::npos) {
            auto newline = text.find('\n', version);
            if (newline != std::string::npos) return text.substr(0, newline + 1) + prefix + text.substr(newline + 1);
        }
        return prefix + text;
    }
    bool draw(SDL_Window* window, int selected, int texture_width, int texture_height, int position, int coordinate,
              U mode, int first, int count, const float* vertices, float target_width, float target_height,
              SDL_Texture* sdl_texture = nullptr, const SDL_Rect* clip = nullptr, bool clipped = false,
              const std::array<float, 4>& source_uv = {0, 0, 1, 1}, bool swap_channels = false,
              const std::array<float, 4>& modulation = {1, 1, 1, 1}, bool alpha_blend = false, bool hd = false,
              bool hd_mask = false, SDL_Texture* hd_rules_texture = nullptr, int grid_width = 0, int grid_height = 0) {
        // HD runs only on the SDL path, which supplies the vertices; it may draw with no filter (selected 0).
        hd = hd && vertices && sdl_texture;
        if (selected < (hd ? 0 : 1) || selected >= shader_count || texture_width <= 0 || texture_height <= 0 ||
            position < 0 || coordinate < 0 || position == coordinate)
            return false;
        static auto proc = overlay_sdl<decltype(&SDL_GL_GetProcAddress)>("SDL_GL_GetProcAddress");
        static auto context = overlay_sdl<decltype(&SDL_GL_GetCurrentContext)>("SDL_GL_GetCurrentContext");
        static auto drawable = overlay_sdl<decltype(&SDL_GL_GetDrawableSize)>("SDL_GL_GetDrawableSize");
        static auto bind_sdl = overlay_sdl<decltype(&SDL_GL_BindTexture)>("SDL_GL_BindTexture");
        if (!proc || !context || (vertices && (!drawable || !bind_sdl))) return false;
        auto* current = context();
        if (!current) return false;
        std::lock_guard<std::mutex> guard(mutex);
        auto& gpu = contexts[current];
        size_t function = 0;
#define GL(name, ret, args)                                                                                            \
    auto& entry_##name = gpu.functions[function++];                                                                    \
    if (!entry_##name) entry_##name = proc("gl" #name);                                                                \
    auto name = reinterpret_cast<ret(*) args>(entry_##name);                                                           \
    if (!name) return false
        GL(GetIntegerv, void, (U, I*));
        GL(GetBooleanv, void, (U, B*));
        GL(IsEnabled, B, (U));
        GL(Enable, void, (U));
        GL(Disable, void, (U));
        GL(CreateShader, U, (U));
        GL(ShaderSource, void, (U, I, const char* const*, const I*));
        GL(CompileShader, void, (U));
        GL(GetShaderiv, void, (U, U, I*));
        GL(GetShaderInfoLog, void, (U, I, I*, char*));
        GL(DeleteShader, void, (U));
        GL(CreateProgram, U, ());
        GL(AttachShader, void, (U, U));
        GL(BindAttribLocation, void, (U, U, const char*));
        GL(LinkProgram, void, (U));
        GL(GetProgramiv, void, (U, U, I*));
        GL(GetProgramInfoLog, void, (U, I, I*, char*));
        GL(DeleteProgram, void, (U));
        GL(UseProgram, void, (U));
        GL(GetUniformLocation, I, (U, const char*));
        GL(Uniform1i, void, (I, I));
        GL(Uniform1f, void, (I, float));
        GL(Uniform2f, void, (I, float, float));
        GL(Uniform4f, void, (I, float, float, float, float));
        GL(BindTexture, void, (U, U));
        GL(TexParameteri, void, (U, U, I));
        GL(GetTexParameteriv, void, (U, U, I*));
        GL(ActiveTexture, void, (U));
        GL(GenBuffers, void, (I, U*));
        GL(BindBuffer, void, (U, U));
        GL(BufferData, void, (U, P, const void*, U));
        GL(BufferSubData, void, (U, P, P, const void*));
        GL(GetVertexAttribiv, void, (U, U, I*));
        GL(GetVertexAttribPointerv, void, (U, U, void**));
        GL(VertexAttribPointer, void, (U, I, U, B, I, const void*));
        GL(EnableVertexAttribArray, void, (U));
        GL(DisableVertexAttribArray, void, (U));
        GL(BlendFuncSeparate, void, (U, U, U, U));
        GL(BlendEquationSeparate, void, (U, U));
        GL(ColorMask, void, (B, B, B, B));
        GL(Viewport, void, (I, I, I, I));
        GL(Scissor, void, (I, I, I, I));
        GL(DrawArrays, void, (U, I, I));
        GL(GenFramebuffers, void, (I, U*));
        GL(BindFramebuffer, void, (U, U));
        GL(FramebufferTexture2D, void, (U, U, U, U, I));
        GL(CheckFramebufferStatus, U, (U));
        GL(GenTextures, void, (I, U*));
        GL(TexImage2D, void, (U, I, I, I, I, I, U, U, const void*));
        GL(DeleteTextures, void, (I, const U*));
        GL(DeleteFramebuffers, void, (I, const U*));
#undef GL
        if (!gpu.precision_known) {
            auto precision = reinterpret_cast<void (*)(U, U, I*, I*)>(proc("glGetShaderPrecisionFormat"));
            I range[2]{}, bits = 0;
            if (precision) precision(0x8b30, 0x8df2, range, &bits);
            gpu.high_precision = bits > 0;
            gpu.precision_known = true;
        }
        // HD: 1 = the preset samples hd2d() itself (one draw, no intermediate image), 2 = it reads gpu.hd.image.
        const int hd_mode = !hd ? 0 : reads_once(selected) ? 1 : 2;
        // A one-draw program compiles the draw's source layout in (hd2d_common's HD_FIXED).
        const bool hd_scaled = texture_width != 256 || texture_height != 192;
        const int hd_layout = hd_mode == 1 ? 1 | hd_scaled << 1 | hd_mask << 2 | swap_channels << 3 : 0;
        const float* strength = hd2d_strength[hd_smooth.load(std::memory_order_relaxed) ? 1 : 0];
        const std::array<int, 5> key{selected, position, coordinate, hd_mode | hd_layout << 4,
                                     grid_width << 16 | grid_height};
        auto found = gpu.programs.find(key);
        if (found == gpu.programs.end()) {
            Program program;
            const auto& text = source(selected);
            if (!text.first.empty() && !text.second.empty()) {
                auto compile = [&](U type, const std::string& body) {
                    auto prepared = prepare(body, gpu.high_precision);
                    if (type == 0x8b30) {
                        // Grid strength for the 640x480 panels; see shader_catalog.hpp.
                        prepared = shader_grid_defines(selected) + prepared;
                        // Another system's screen (RG DS Play): the presets' DS cell grid becomes its own.
                        const std::string ds_grid = "vec2(256.0,192.0)";
                        if (grid_width > 0 && grid_height > 0)
                            for (size_t at = 0; (at = prepared.find(ds_grid, at)) != std::string::npos;) {
                                const std::string grid = "vec2(" + std::to_string(grid_width) + ".0," +
                                                         std::to_string(grid_height) + ".0)";
                                prepared.replace(at, ds_grid.size(), grid);
                                at += grid.size();
                            }
                        const std::string sample = "texture2D(u_texture,";
                        size_t offset = 0;
                        while ((offset = prepared.find(sample, offset)) != std::string::npos) {
                            prepared.replace(offset, sample.size(), "rgds_sample(");
                            offset += 12;
                        }
                        const std::string declaration = "uniform vec4 u_texture_size;";
                        auto at = prepared.find(declaration);
                        const bool linear = shader_uses_target_size(selected);
                        // Quantize nearest samples before mapping into a combined
                        // texture. This avoids floating-point ties choosing a
                        // different texel at half-texture/source-region boundaries.
                        // HD: the source is the reconstruction at the output's own size, read as it is.
                        std::string sample_body =
                            hd_mode  ? ""
                            : linear ? "uv=clamp(uv,0.5*u_texture_size.xy,vec2(1.0)-0.5*u_texture_size.xy);"
                                     : "uv=(clamp(floor(uv*u_texture_size.zw),vec2(0.0),u_texture_size.zw-1.0)+0.5)*u_texture_size.xy;";
                        const std::string sampler =
                            hd_mode == 1 ? "#define HD_FIXED\n#define HD_SCALED " + std::to_string(int(hd_scaled)) +
                                               "\n#define HD_MASK " + std::to_string(int(hd_mask)) +
                                               "\n#define HD_SWAP " + std::to_string(int(swap_channels)) + "\n" +
                                               hd2d_common + hd2d_sample +
                                               "\nvec4 rgds_sample(vec2 uv){return hd2d(uv);}\n"
                                         : "vec4 rgds_sample(vec2 uv){" + sample_body +
                                               "vec4 pixel=texture2D(u_texture,u_rgds_source.xy+uv*u_rgds_source.zw);return mix(pixel,pixel.bgra,u_rgds_swap); }\n";
                        if (at != std::string::npos)
                            prepared.insert(at + declaration.size(),
                                            "\nuniform vec4 u_rgds_source;uniform float u_rgds_swap;uniform vec4 u_rgds_modulation;\n" +
                                                sampler);
                    }
                    if (type == 0x8b30) {
                        auto main = prepared.find("void main()");
                        if (main != std::string::npos) {
                            prepared.replace(main, 11, "void rgds_main()");
                            prepared += "\nvoid main(){rgds_main();gl_FragColor*=u_rgds_modulation;}\n";
                        }
                    }
                    U shader = CreateShader(type);
                    const char* data = prepared.c_str();
                    ShaderSource(shader, 1, &data, nullptr);
                    CompileShader(shader);
                    I ok = 0;
                    GetShaderiv(shader, 0x8b81, &ok);
                    if (!ok) {
                        char log[2048]{};
                        GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                        std::fprintf(stderr, "RGDS shader %d %s compile: %s\n", selected,
                                     type == 0x8b31 ? "vertex" : "fragment", log);
                        DeleteShader(shader);
                        return U(0);
                    }
                    return shader;
                };
                U vs = compile(0x8b31, text.first), fs = compile(0x8b30, text.second);
                if (vs && fs) {
                    U id = CreateProgram();
                    AttachShader(id, vs);
                    AttachShader(id, fs);
                    BindAttribLocation(id, position, "a_vertex_coordinate");
                    BindAttribLocation(id, coordinate, "a_texture_coordinate");
                    LinkProgram(id);
                    I linked = 0;
                    GetProgramiv(id, 0x8b82, &linked);
                    if (linked) {
                        program = {id,
                                   GetUniformLocation(id, "u_texture"),
                                   GetUniformLocation(id, "u_texture_size"),
                                   GetUniformLocation(id, "u_target_size"),
                                   GetUniformLocation(id, "u_rgds_source"),
                                   GetUniformLocation(id, "u_rgds_swap"),
                                   GetUniformLocation(id, "u_rgds_modulation")};
                        if (hd_mode == 1) {
                            program.hd_src = GetUniformLocation(id, "u_hd_src");
                            program.hd_rect = GetUniformLocation(id, "u_hd_rect");
                            program.hd_px = GetUniformLocation(id, "u_hd_px");
                            program.hd_swap = GetUniformLocation(id, "u_hd_swap");
                            program.hd_mask = GetUniformLocation(id, "u_hd_mask");
                            program.hd_rules = GetUniformLocation(id, "u_hd_rules");
                            program.hd_out = GetUniformLocation(id, "u_hd_out");
                            program.hd_edge = GetUniformLocation(id, "u_hd_edge");
                            program.hd_smooth = GetUniformLocation(id, "u_hd_smooth");
                            program.hd_soft = GetUniformLocation(id, "u_hd_soft");
                        }
                    } else {
                        char log[2048]{};
                        GetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
                        std::fprintf(stderr, "RGDS shader %d link: %s\n", selected, log);
                        DeleteProgram(id);
                    }
                }
                if (vs) DeleteShader(vs);
                if (fs) DeleteShader(fs);
            }
            found = gpu.programs.emplace(key, program).first;
        }
        auto& program = found->second;
        if (!program.id) {
            if (hd && !gpu.hd.said_failed) {
                gpu.hd.said_failed = true;
                std::fprintf(stderr, "RGDS HD 2D: no program for filter %d; showing the plain picture\n", selected);
            }
            return false;
        }
        // HD 2D passes, from the SDL texture on unit 0: pass 1 (rules, 256x192, only when DSCore gave none) and pass 2
        // (the reconstruction into gpu.hd.image, only for presets that read the picture more than once). They change
        // the framebuffer, program, buffer and attribute bindings; the caller restores them. A failure turns HD off
        // for this context and the frame is drawn the plain way.
        auto hd_target = [&](U& texture, U& framebuffer, int width, int height) -> bool {
            if (!texture) GenTextures(1, &texture);
            BindTexture(0x0de1, texture);
            TexImage2D(0x0de1, 0, 0x1908, width, height, 0, 0x1908, 0x1401, nullptr);
            for (U name : {0x2801u, 0x2800u}) TexParameteri(0x0de1, name, 0x2600); // nearest
            for (U name : {0x2802u, 0x2803u}) TexParameteri(0x0de1, name, 0x812f); // clamp to edge
            if (!framebuffer) GenFramebuffers(1, &framebuffer);
            BindFramebuffer(0x8d40, framebuffer);
            FramebufferTexture2D(0x8d40, 0x8ce0, 0x0de1, texture, 0);
            return CheckFramebufferStatus(0x8d40) == 0x8cd5;
        };
        auto hd_ready = [&]() -> bool {
            Hd& hd_state = gpu.hd;
            if (hd_state.failed) return false;
            if (hd_state.rules) return true;
            auto build = [&](const std::string& fragment) -> U {
                auto compile = [&](U type, const std::string& body) -> U {
                    U shader = CreateShader(type);
                    const char* data = body.c_str();
                    ShaderSource(shader, 1, &data, nullptr);
                    CompileShader(shader);
                    I ok = 0;
                    GetShaderiv(shader, 0x8b81, &ok);
                    if (!ok) {
                        char log[2048]{};
                        GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                        std::fprintf(stderr, "RGDS HD 2D shader compile: %s\n", log);
                        DeleteShader(shader);
                        return 0;
                    }
                    return shader;
                };
                U vs = compile(0x8b31, hd2d_vertex), fs = compile(0x8b30, std::string(hd2d_common) + fragment);
                U id = 0;
                if (vs && fs) {
                    id = CreateProgram();
                    AttachShader(id, vs);
                    AttachShader(id, fs);
                    BindAttribLocation(id, 0, "a_position");
                    LinkProgram(id);
                    I linked = 0;
                    GetProgramiv(id, 0x8b82, &linked);
                    if (!linked) {
                        char log[2048]{};
                        GetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
                        std::fprintf(stderr, "RGDS HD 2D shader link: %s\n", log);
                        DeleteProgram(id);
                        id = 0;
                    }
                }
                if (vs) DeleteShader(vs);
                if (fs) DeleteShader(fs);
                return id;
            };
            hd_state.rules = build(hd2d_rules);
            hd_state.reconstruct = build(std::string(hd2d_sample) + hd2d_reconstruct);
            const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
            GenBuffers(1, &hd_state.quad);
            BindBuffer(0x8892, hd_state.quad);
            BufferData(0x8892, sizeof(quad), quad, 0x88e4);
            if (!hd_state.rules || !hd_state.reconstruct || !hd_state.quad ||
                !hd_target(hd_state.rules_texture, hd_state.rules_target, 256, 192)) {
                hd_state.failed = true;
                std::fprintf(stderr, "RGDS HD 2D: unavailable on this GPU; showing the plain picture\n");
                return false;
            }
            return true;
        };
        auto hd_uniforms = [&](U id, U source) {
            UseProgram(id);
            Uniform1i(GetUniformLocation(id, "u_hd_src"), 0);
            Uniform4f(GetUniformLocation(id, "u_hd_rect"), source_uv[0], source_uv[1], source_uv[2], source_uv[3]);
            Uniform2f(GetUniformLocation(id, "u_hd_px"), float(texture_width), float(texture_height));
            Uniform1f(GetUniformLocation(id, "u_hd_swap"), swap_channels ? 1.f : 0.f);
            Uniform1f(GetUniformLocation(id, "u_hd_mask"), hd_mask ? 1.f : 0.f);
            Uniform1f(GetUniformLocation(id, "u_hd_edge"), strength[0]);
            Uniform1f(GetUniformLocation(id, "u_hd_smooth"), strength[1]);
            Uniform1f(GetUniformLocation(id, "u_hd_soft"), strength[2]);
            ActiveTexture(0x84c0);
            BindTexture(0x0de1, source);
        };
        auto hd_quad = [&] {
            BindBuffer(0x8892, gpu.hd.quad);
            VertexAttribPointer(0, 2, 0x1406, 0, 0, nullptr);
            EnableVertexAttribArray(0);
            DisableVertexAttribArray(1);
        };
        auto hd_rules_pass = [&](U source) -> bool {
            if (!hd_ready()) return false;
            hd_quad();
            BindFramebuffer(0x8d40, gpu.hd.rules_target);
            Viewport(0, 0, 256, 192);
            hd_uniforms(gpu.hd.rules, source);
            DrawArrays(5, 0, 4);
            return true;
        };
        auto hd_image_pass = [&](int out_w, int out_h, U source, U rules) -> bool {
            Hd& hd_state = gpu.hd;
            if (!hd_ready() || out_w <= 0 || out_h <= 0 || out_w > 4096 || out_h > 4096) return false;
            if (out_w != hd_state.width || out_h != hd_state.height) {
                if (!hd_target(hd_state.image, hd_state.image_target, out_w, out_h)) {
                    hd_state.failed = true;
                    std::fprintf(stderr, "RGDS HD 2D: no %dx%d render target; showing the plain picture\n", out_w,
                                 out_h);
                    return false;
                }
                hd_state.width = out_w;
                hd_state.height = out_h;
            }
            hd_quad();
            BindFramebuffer(0x8d40, hd_state.image_target);
            Viewport(0, 0, out_w, out_h);
            hd_uniforms(hd_state.reconstruct, source);
            Uniform1i(GetUniformLocation(hd_state.reconstruct, "u_hd_rules"), 1);
            Uniform2f(GetUniformLocation(hd_state.reconstruct, "u_hd_out"), float(out_w), float(out_h));
            ActiveTexture(0x84c1);
            BindTexture(0x0de1, rules);
            ActiveTexture(0x84c0);
            DrawArrays(5, 0, 4);
            return true;
        };
        I old_program = 0, old_active = 0, old_texture = 0, old_min = 0, old_mag = 0, viewport[4]{};
        GetIntegerv(0x8b8d, &old_program);
        GetIntegerv(0x84e0, &old_active);
        ActiveTexture(0x84c0);
        GetIntegerv(0x8069, &old_texture);
        GetIntegerv(0x0ba2, viewport);
        bool drawn = true; // false when HD could not draw: the caller then draws the plain way
        // HD: DSCore's rules for this frame, uploaded with it (their GL name; read nearest).
        U cpu_rules = 0;
        if (hd && hd_rules_texture) {
            float rw = 1, rh = 1;
            I bound = 0;
            if (bind_sdl(hd_rules_texture, &rw, &rh) == 0 && rw == 1.f && rh == 1.f) {
                GetIntegerv(0x8069, &bound);
                cpu_rules = U(bound);
                for (U name : {0x2801u, 0x2800u}) TexParameteri(0x0de1, name, 0x2600);
            }
        }
        // SDL_GL_BindTexture flushes queued draws and selects the native game
        // texture without readback. GLES BGR conversion is applied by our shader.
        if (sdl_texture) {
            float tw = 1, th = 1;
            if (bind_sdl(sdl_texture, &tw, &th) != 0 || tw != 1.f || th != 1.f) {
                BindTexture(0x0de1, old_texture);
                ActiveTexture(old_active);
                return false;
            }
        }
        I image = 0;
        GetIntegerv(0x8069, &image);
        if (!image) {
            BindTexture(0x0de1, old_texture);
            ActiveTexture(old_active);
            return false;
        }
        GetTexParameteriv(0x0de1, 0x2801, &old_min);
        GetTexParameteriv(0x0de1, 0x2800, &old_mag);
        const I filter = shader_uses_target_size(selected) ? 0x2601 : 0x2600;
        if (old_min != filter) TexParameteri(0x0de1, 0x2801, filter);
        if (old_mag != filter) TexParameteri(0x0de1, 0x2800, filter);
        UseProgram(program.id);
        Uniform1i(program.image, 0);
        Uniform4f(program.size, 1.f / texture_width, 1.f / texture_height, float(texture_width), float(texture_height));
        if (hd) Uniform4f(program.source, 0, 0, 1, 1);
        else Uniform4f(program.source, source_uv[0], source_uv[1], source_uv[2], source_uv[3]);
        Uniform1f(program.swap, swap_channels && !hd ? 1.f : 0.f);
        Uniform4f(program.modulation, modulation[0], modulation[1], modulation[2], modulation[3]);
        Uniform2f(program.target, target_width > 0 ? target_width : float(viewport[2]),
                  target_height > 0 ? target_height : float(viewport[3]));
        if (vertices) {
            I w = 0, h = 0;
            drawable(window, &w, &h);
            I old_buffer = 0, old_scissor[4]{}, blend[6]{};
            B mask[4]{};
            const U blend_names[] = {0x80c9, 0x80c8, 0x80cb, 0x80ca, 0x8009, 0x883d};
            if (alpha_blend)
                for (int i = 0; i < 6; i++) GetIntegerv(blend_names[i], &blend[i]);
            GetIntegerv(0x8894, &old_buffer);
            GetIntegerv(0x0c10, old_scissor);
            GetBooleanv(0x0c23, mask);
            const U modes[] = {0x0be2, 0x0c11, 0x0b71, 0x0b90, 0x0b44};
            B enabled[5]{};
            for (int i = 0; i < 5; i++) {
                enabled[i] = IsEnabled(modes[i]);
                Disable(modes[i]);
            }
            struct Attribute {
                I size, type, normalized, stride, buffer, enabled;
                void* pointer;
            };
            Attribute attributes[2]{};
            for (U i = 0; i < 2; i++) {
                auto& a = attributes[i];
                GetVertexAttribiv(i, 0x8623, &a.size);
                GetVertexAttribiv(i, 0x8625, &a.type);
                GetVertexAttribiv(i, 0x886a, &a.normalized);
                GetVertexAttribiv(i, 0x8624, &a.stride);
                GetVertexAttribiv(i, 0x889f, &a.buffer);
                GetVertexAttribiv(i, 0x8622, &a.enabled);
                GetVertexAttribPointerv(i, 0x8645, &a.pointer);
            }
            ColorMask(1, 1, 1, 1);
            I old_unit1 = 0;
            if (hd) {
                const int out_w = int(std::lround(target_width)), out_h = int(std::lround(target_height));
                I old_target = 0;
                GetIntegerv(0x8ca6, &old_target);
                U rules = cpu_rules;
                if (!rules && hd_rules_pass(U(image))) rules = gpu.hd.rules_texture;
                drawn = rules && (hd_mode == 1 || hd_image_pass(out_w, out_h, U(image), rules));
                BindFramebuffer(0x8d40, U(old_target));
                ActiveTexture(0x84c1);
                GetIntegerv(0x8069, &old_unit1);
                if (hd_mode == 1) BindTexture(0x0de1, rules);
                ActiveTexture(0x84c0);
                BindTexture(0x0de1, hd_mode == 2 ? gpu.hd.image : U(image));
                UseProgram(program.id);
                if (hd_mode == 1) {
                    Uniform1i(program.hd_src, 0);
                    Uniform1i(program.hd_rules, 1);
                    Uniform4f(program.hd_rect, source_uv[0], source_uv[1], source_uv[2], source_uv[3]);
                    Uniform2f(program.hd_px, float(texture_width), float(texture_height));
                    Uniform1f(program.hd_swap, swap_channels ? 1.f : 0.f);
                    Uniform1f(program.hd_mask, hd_mask ? 1.f : 0.f);
                    Uniform2f(program.hd_out, float(out_w), float(out_h));
                    Uniform1f(program.hd_edge, strength[0]);
                    Uniform1f(program.hd_smooth, strength[1]);
                    Uniform1f(program.hd_soft, strength[2]);
                }
            }
            Viewport(0, 0, w, h);
            if (alpha_blend) {
                Enable(0x0be2);
                BlendEquationSeparate(0x8006, 0x8006);
                BlendFuncSeparate(0x0302, 0x0303, 1, 0x0303);
            }
            if (clipped && clip) {
                Enable(0x0c11);
                Scissor(clip->x, h - clip->y - clip->h, clip->w, clip->h);
            }
            if (!gpu.buffer) {
                GenBuffers(1, &gpu.buffer);
                BindBuffer(0x8892, gpu.buffer);
                BufferData(0x8892, 16 * sizeof(float), vertices, 0x88e8);
                std::copy(vertices, vertices + 16, gpu.vertices.begin());
                gpu.vertices_known = true;
            } else {
                BindBuffer(0x8892, gpu.buffer);
                if (!gpu.vertices_known || !std::equal(vertices, vertices + 16, gpu.vertices.begin())) {
                    BufferSubData(0x8892, 0, 16 * sizeof(float), vertices);
                    std::copy(vertices, vertices + 16, gpu.vertices.begin());
                    gpu.vertices_known = true;
                }
            }
            VertexAttribPointer(0, 2, 0x1406, 0, 4 * sizeof(float), nullptr);
            VertexAttribPointer(1, 2, 0x1406, 0, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
            EnableVertexAttribArray(0);
            EnableVertexAttribArray(1);
            if (drawn) DrawArrays(5, 0, 4);
            if (hd && drawn && !gpu.hd.said_drawing) {
                gpu.hd.said_drawing = true;
                std::fprintf(stderr, "RGDS HD 2D: drawing %dx%d from %dx%d, %s, %s rules, filter %d\n",
                             int(std::lround(target_width)), int(std::lround(target_height)), texture_width,
                             texture_height, hd_mode == 1 ? "in one pass" : "through an image",
                             cpu_rules ? "CPU" : "GPU", selected);
            } else if (hd && !drawn && !gpu.hd.said_failed) {
                gpu.hd.said_failed = true;
                std::fprintf(stderr, "RGDS HD 2D: could not draw (%s rules); showing the plain picture\n",
                             cpu_rules ? "CPU" : "GPU");
            }
            if (hd) {
                ActiveTexture(0x84c1);
                BindTexture(0x0de1, U(old_unit1));
                ActiveTexture(0x84c0);
            }
            for (U i = 0; i < 2; i++) {
                auto& a = attributes[i];
                BindBuffer(0x8892, a.buffer);
                VertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, a.pointer);
                if (a.enabled) EnableVertexAttribArray(i);
                else DisableVertexAttribArray(i);
            }
            BindBuffer(0x8892, old_buffer);
            Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            Scissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
            ColorMask(mask[0], mask[1], mask[2], mask[3]);
            if (alpha_blend) {
                BlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
                BlendEquationSeparate(blend[4], blend[5]);
            }
            for (int i = 0; i < 5; i++) {
                if (enabled[i]) Enable(modes[i]);
                else Disable(modes[i]);
            }
        } else DrawArrays(mode, first, count);
        if (hd) BindTexture(0x0de1, U(image)); // the filters below belong to the SDL texture, not the HD image
        if (old_min != filter) TexParameteri(0x0de1, 0x2801, old_min);
        if (old_mag != filter) TexParameteri(0x0de1, 0x2800, old_mag);
        BindTexture(0x0de1, old_texture);
        ActiveTexture(old_active);
        UseProgram(old_program);
        return drawn;
    }

public:
    // HD: `rules` (256x192, R G B A bytes as hd2d_rules writes them) holds the CPU's rules for whatever frame
    // `image` holds; null forgets the pair (the GPU then computes the rules).
    // HD smooth (rgds_2d=5) instead of HD: the strength hd2d_strength[1]; dscore's rules must match it.
    std::atomic<bool> hd_smooth{false};
    void pair_rules(SDL_Texture* image, SDL_Texture* rules) {
        std::lock_guard<std::mutex> guard(pairs_mutex);
        if (rules) rules_for[image] = rules;
        else rules_for.erase(image);
    }
    static bool ds_texture(int width, int height) {
        return width >= 256 && width <= 1024 && width % 256 == 0 && height == width * 3 / 4;
    }
    static bool screen_texture(int width, int height) {
        return ds_texture(width, height) || (height % 2 == 0 && ds_texture(width, height / 2)) ||
               (width % 2 == 0 && ds_texture(width / 2, height));
    }
    // RG DS Play's game screens: Game Boy Advance, and Game Boy / Game Boy Color.
    static bool handheld_texture(int width, int height) {
        return (width == 240 && height == 160) || (width == 160 && height == 144);
    }
    void destroy(SDL_GLContext context) {
        std::lock_guard<std::mutex> guard(mutex);
        auto found = contexts.find(context);
        if (found == contexts.end()) return;
        static auto current = overlay_sdl<decltype(&SDL_GL_GetCurrentContext)>("SDL_GL_GetCurrentContext");
        static auto proc = overlay_sdl<decltype(&SDL_GL_GetProcAddress)>("SDL_GL_GetProcAddress");
        if (current && proc && current() == context) {
            auto programs = reinterpret_cast<void (*)(U)>(proc("glDeleteProgram"));
            auto buffers = reinterpret_cast<void (*)(I, const U*)>(proc("glDeleteBuffers"));
            if (programs)
                for (auto& program : found->second.programs)
                    if (program.second.id) programs(program.second.id);
            if (buffers && found->second.buffer) buffers(1, &found->second.buffer);
            const Hd& hd = found->second.hd;
            auto textures = reinterpret_cast<void (*)(I, const U*)>(proc("glDeleteTextures"));
            auto framebuffers = reinterpret_cast<void (*)(I, const U*)>(proc("glDeleteFramebuffers"));
            if (programs)
                for (U id : {hd.rules, hd.reconstruct})
                    if (id) programs(id);
            if (buffers && hd.quad) buffers(1, &hd.quad);
            if (textures)
                for (U id : {hd.rules_texture, hd.image})
                    if (id) textures(1, &id);
            if (framebuffers)
                for (U id : {hd.rules_target, hd.image_target})
                    if (id) framebuffers(1, &id);
        }
        contexts.erase(found);
    }
    // NNDDSS's core native screen blit already supplies clip-space vertices.
    // Reuse its VBO/attributes and draw command, restoring its program/filter.
    bool native(int selected, int width, int height, int position, int coordinate, U mode, int first, int count,
                float target_width = 0, float target_height = 0) {
        return ds_texture(width, height) && draw(nullptr, selected, width, height, position, coordinate, mode, first,
                                                 count, nullptr, target_width, target_height);
    }
    bool sdl(SDL_Renderer* renderer, SDL_Texture* texture, int selected, const SDL_Rect* source, const SDL_Rect* target,
             double angle = 0, const SDL_Point* center = nullptr, SDL_RendererFlip flip = SDL_FLIP_NONE,
             const SDL_FPoint* exact_center = nullptr, bool enhanced = false, bool hd = false,
             bool handheld = false, int grid_width = 0, int grid_height = 0) {
        // grid_width/height: a picture made of cells of that many across and down (RG DS Play's second
        // screen, drawn at the panel's size), shaded as a screen of that many pixels.
        static auto query = overlay_sdl<decltype(&SDL_QueryTexture)>("SDL_QueryTexture");
        static auto window = overlay_sdl<decltype(&SDL_RenderGetWindow)>("SDL_RenderGetWindow");
        static auto info = overlay_sdl<decltype(&SDL_GetRendererInfo)>("SDL_GetRendererInfo");
        static auto get_target = overlay_sdl<decltype(&SDL_GetRenderTarget)>("SDL_GetRenderTarget");
        static auto drawable = overlay_sdl<decltype(&SDL_GL_GetDrawableSize)>("SDL_GL_GetDrawableSize");
        static auto scale = overlay_sdl<decltype(&SDL_RenderGetScale)>("SDL_RenderGetScale");
        static auto viewport = overlay_sdl<decltype(&SDL_RenderGetViewport)>("SDL_RenderGetViewport");
        static auto get_clip = overlay_sdl<decltype(&SDL_RenderGetClipRect)>("SDL_RenderGetClipRect");
        static auto is_clipped = overlay_sdl<decltype(&SDL_RenderIsClipEnabled)>("SDL_RenderIsClipEnabled");
        static auto bind = overlay_sdl<decltype(&SDL_GL_BindTexture)>("SDL_GL_BindTexture");
        static auto flush = overlay_sdl<decltype(&SDL_RenderFlush)>("SDL_RenderFlush");
        static auto get_color = overlay_sdl<decltype(&SDL_GetTextureColorMod)>("SDL_GetTextureColorMod");
        static auto get_alpha = overlay_sdl<decltype(&SDL_GetTextureAlphaMod)>("SDL_GetTextureAlphaMod");
        static auto get_blend = overlay_sdl<decltype(&SDL_GetTextureBlendMode)>("SDL_GetTextureBlendMode");
        if (!query || !window || !info || !get_target || !drawable || !scale || !viewport || !bind || !flush ||
            !get_color || !get_alpha || !get_blend || get_target(renderer))
            return false;
        Uint32 format = 0;
        int tw = 0, th = 0;
        if (query(texture, &format, nullptr, &tw, &th) != 0 ||
            !(screen_texture(tw, th) || (enhanced && tw == 640 && th == 480) || (handheld && handheld_texture(tw, th)) ||
              (handheld && grid_width > 0 && grid_height > 0)))
            return false;
        SDL_RendererInfo driver{};
        if (info(renderer, &driver) != 0 || !driver.name ||
            (std::strcmp(driver.name, "opengl") && std::strcmp(driver.name, "opengles2")))
            return false;
        auto output = window(renderer);
        if (!output) return false;
        // BindTexture only flushes commands that reference THIS texture. A
        // queued RenderClear must run before the replacement shader draw too.
        if (flush(renderer) != 0) return false;
        float unused_w = 1, unused_h = 1;
        if (bind(texture, &unused_w, &unused_h) != 0) return false;
        Uint8 red = 255, green = 255, blue = 255, alpha = 255;
        SDL_BlendMode blend_mode = SDL_BLENDMODE_NONE;
        if (get_color(texture, &red, &green, &blue) != 0 || get_alpha(texture, &alpha) != 0 ||
            get_blend(texture, &blend_mode) != 0 ||
            (blend_mode != SDL_BLENDMODE_NONE && blend_mode != SDL_BLENDMODE_BLEND))
            return false;
        const std::array<float, 4> modulation{red / 255.f, green / 255.f, blue / 255.f, alpha / 255.f};
        const bool swap_channels = !std::strcmp(driver.name, "opengles2") &&
                                   (format == SDL_PIXELFORMAT_BGRA32 || format == SDL_PIXELFORMAT_BGRX32);
        int w = 0, h = 0;
        drawable(output, &w, &h);
        if (w <= 0 || h <= 0) return false;
        float sx = 1, sy = 1;
        scale(renderer, &sx, &sy);
        if (sx <= 0 || sy <= 0) return false;
        SDL_Rect view{};
        viewport(renderer, &view);
        SDL_Rect src = source ? *source : SDL_Rect{0, 0, tw, th};
        SDL_Rect dst = target ? *target : SDL_Rect{0, 0, view.w, view.h};
        if (dst.w <= 0 || dst.h <= 0 || src.w <= 0 || src.h <= 0 || src.x < 0 || src.y < 0 || src.x + src.w > tw ||
            src.y + src.h > th)
            return false;
        const bool other_system = handheld && (handheld_texture(src.w, src.h) || (grid_width > 0 && grid_height > 0));
        if (other_system && (grid_width <= 0 || grid_height <= 0)) {
            grid_width = src.w;
            grid_height = src.h;
        }
        if (!ds_texture(src.w, src.h) && !(enhanced && src.w == 640 && src.h == 480) && !other_system) {
            // Give each screen its own native grid/target scale, then rotate
            // both quads around the same original centre.
            SDL_Rect first = src, second = src, left = dst, right = dst;
            if (ds_texture(src.w, src.h / 2) && src.h % 2 == 0) {
                first.h = second.h = src.h / 2;
                second.y += first.h;
                left.h = dst.h / 2;
                right.y += left.h;
                right.h = dst.h - left.h;
                if (flip & SDL_FLIP_VERTICAL) std::swap(left, right);
            } else if (ds_texture(src.w / 2, src.h) && src.w % 2 == 0) {
                first.w = second.w = src.w / 2;
                second.x += first.w;
                left.w = dst.w / 2;
                right.x += left.w;
                right.w = dst.w - left.w;
                if (flip & SDL_FLIP_HORIZONTAL) std::swap(left, right);
            } else return false;
            const float cx = exact_center ? exact_center->x
                             : center     ? float(center->x)
                                          : dst.w * .5f,
                        cy = exact_center ? exact_center->y
                             : center     ? float(center->y)
                                          : dst.h * .5f;
            SDL_FPoint first_center{cx - (left.x - dst.x), cy - (left.y - dst.y)},
                second_center{cx - (right.x - dst.x), cy - (right.y - dst.y)};
            return sdl(renderer, texture, selected, &first, &left, angle, nullptr, flip, &first_center, false, hd) &&
                   sdl(renderer, texture, selected, &second, &right, angle, nullptr, flip, &second_center, false, hd);
        }
        const double radians = angle * 3.14159265358979323846 / 180.0, c = std::cos(radians), s = std::sin(radians);
        const float cx = exact_center ? exact_center->x
                         : center     ? float(center->x)
                                      : dst.w * .5f,
                    cy = exact_center ? exact_center->y
                         : center     ? float(center->y)
                                      : dst.h * .5f;
        float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
        const std::array<float, 4> source_uv{float(src.x) / tw, float(src.y) / th, float(src.w) / tw,
                                             float(src.h) / th};
        if (flip & SDL_FLIP_HORIZONTAL) std::swap(u0, u1);
        if (flip & SDL_FLIP_VERTICAL) std::swap(v0, v1);
        std::array<float, 16> vertices{};
        const float x[] = {0, float(dst.w), 0, float(dst.w)}, y[] = {0, 0, float(dst.h), float(dst.h)},
                    u[] = {u0, u1, u0, u1}, v[] = {v0, v0, v1, v1};
        for (int i = 0; i < 4; i++) {
            const float px = float((x[i] - cx) * c - (y[i] - cy) * s + cx + dst.x + view.x) * sx;
            const float py = float((x[i] - cx) * s + (y[i] - cy) * c + cy + dst.y + view.y) * sy;
            vertices[i * 4] = 2 * px / w - 1;
            vertices[i * 4 + 1] = 1 - 2 * py / h;
            vertices[i * 4 + 2] = u[i];
            vertices[i * 4 + 3] = v[i];
        }
        SDL_Rect clip{int(std::floor(view.x * sx)), int(std::floor(view.y * sy)), int(std::ceil(view.w * sx)),
                      int(std::ceil(view.h * sy))};
        if (get_clip && is_clipped && is_clipped(renderer)) {
            SDL_Rect local{};
            get_clip(renderer, &local);
            SDL_Rect requested{int(std::floor((view.x + local.x) * sx)), int(std::floor((view.y + local.y) * sy)),
                               int(std::ceil(local.w * sx)), int(std::ceil(local.h * sy))};
            int right = std::min(clip.x + clip.w, requested.x + requested.w),
                bottom = std::min(clip.y + clip.h, requested.y + requested.h);
            clip.x = std::max(clip.x, requested.x);
            clip.y = std::max(clip.y, requested.y);
            clip.w = std::max(0, right - clip.x);
            clip.h = std::max(0, bottom - clip.y);
        }
        // HD on DSCore's 640x480 3D compose: blue's lowest bit marks the pixels 2D owns.
        SDL_Texture* rules = nullptr;
        if (hd) {
            std::lock_guard<std::mutex> guard(pairs_mutex);
            auto paired = rules_for.find(texture);
            if (paired != rules_for.end()) rules = paired->second;
        }
        return draw(output, selected, src.w, src.h, 0, 1, 5, 0, 4, vertices.data(), dst.w * sx, dst.h * sy, texture,
                    &clip, true, source_uv, swap_channels, modulation, blend_mode == SDL_BLENDMODE_BLEND, hd,
                    hd && enhanced && src.w == 640 && src.h == 480, rules, other_system ? grid_width : 0,
                    other_system ? grid_height : 0);
    }
};
}
