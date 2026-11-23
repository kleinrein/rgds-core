//! Drawing on the player's own GLES context (libretro's hardware rendering): wgpu adopts the context the player made
//! current, Ruffle draws into a texture there, and the picture is blitted into the player's framebuffer, so it never
//! comes back to the CPU. On the RG DS's Panfrost a read-back through wgpu cost 60-70 ms a picture (the GPU drawing
//! itself 2-3 ms); the player either blits the framebuffer into the panel's texture or reads it once with
//! glReadPixels.
//!
//! The player's framebuffer holds the picture from its first row at y 0 (bottom_left_origin false), as wgpu keeps a
//! texture: the blit copies it as it is.

use std::ffi::{CString, c_char, c_void};
use std::sync::Arc;

use glow::HasContext;
use ruffle_render_wgpu::descriptors::Descriptors;
use ruffle_render_wgpu::wgpu;

use crate::libretro as lr;

/// The calls the player gave for its context.
#[derive(Clone, Copy)]
pub struct Host {
    pub framebuffer: lr::HwGetCurrentFramebufferFn,
    pub proc_address: lr::HwGetProcAddressFn,
}

impl Host {
    fn proc(&self, name: &str) -> *const c_void {
        let Ok(name) = CString::new(name) else { return std::ptr::null() };
        unsafe { (self.proc_address)(name.as_ptr() as *const c_char) }.map_or(std::ptr::null(), |f| f as *const c_void)
    }
}

/// wgpu on the context current now (the player's).
pub fn adopt(host: Host) -> Result<Arc<Descriptors>, String> {
    let instance =
        ruffle_render_wgpu::backend::create_wgpu_instance(wgpu::Backends::GL, wgpu::BackendOptions::default(), None);
    let exposed =
        unsafe { wgpu::hal::gles::Adapter::new_external(|name| host.proc(name), wgpu::GlBackendOptions::default()) }
            .ok_or("wgpu cannot use the player's GLES context")?;
    let adapter = unsafe { instance.create_adapter_from_hal(exposed) };
    // Ruffle's own request (ruffle_render_wgpu::backend::request_device): GLES 3.0's limits, raised to the adapter's.
    let mut limits = wgpu::Limits::downlevel_webgl2_defaults();
    limits = limits.using_resolution(adapter.limits());
    limits = limits.using_alignment(adapter.limits());
    limits.max_uniform_buffer_binding_size = adapter.limits().max_uniform_buffer_binding_size;
    limits.max_inter_stage_shader_variables = adapter.limits().max_inter_stage_shader_variables;
    limits.max_color_attachments = 4;
    let optional = wgpu::Features::TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES
        | wgpu::Features::TEXTURE_COMPRESSION_BC
        | wgpu::Features::FLOAT32_FILTERABLE;
    let (device, queue) = futures::executor::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        label: None,
        required_features: optional & adapter.features(),
        required_limits: limits,
        memory_hints: Default::default(),
        trace: wgpu::Trace::Off,
        experimental_features: wgpu::ExperimentalFeatures::disabled(),
    }))
    .map_err(|e| format!("no wgpu device on the player's context: {e}"))?;
    tracing::info!("flash: drawing on the player's context, {:?}", adapter.get_info());
    Ok(Arc::new(Descriptors::new(instance, adapter, device, queue)))
}

/// The picture into the player's framebuffer, with GL calls of its own on the same context: a blit, or the FXAA pass.
pub struct Output {
    host: Host,
    gl: glow::Context,
    read: Option<glow::Framebuffer>,
    fxaa: Option<Fxaa>,
    /// The FXAA program failed to build: blitted from then on.
    no_fxaa: bool,
}

/// FXAA (after Timothy Lottes' FXAA 3.11, the PC's lowest preset): edges found by luma and blended along them, one
/// pass over the picture. Ruffle's own anti-aliasing is MSAA, which wgpu's GL backend does with a multisampled target
/// and a resolve for every blend layer, mask and filter: on the RG DS's Panfrost that made a HappyWheels level 30 ms of
/// drawing a frame, against 1.1 ms without it. FXAA over the finished picture costs about a millisecond.
struct Fxaa {
    program: glow::Program,
    vao: glow::VertexArray,
    sampler: glow::Sampler,
    inv: Option<glow::UniformLocation>,
}

const FXAA_VERTEX: &str = "#version 300 es
void main() {
    // One triangle over the whole viewport.
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
";

const FXAA_FRAGMENT: &str = "#version 300 es
precision mediump float;
uniform sampler2D picture;
uniform vec2 inv; // 1 / the picture's size
out vec4 colour;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
    // The framebuffer's row y is the texture's row y (both from the picture's top): no flip.
    vec2 at = gl_FragCoord.xy * inv;
    vec3 nw = texture(picture, at + vec2(-1.0, -1.0) * inv).rgb;
    vec3 ne = texture(picture, at + vec2(1.0, -1.0) * inv).rgb;
    vec3 sw = texture(picture, at + vec2(-1.0, 1.0) * inv).rgb;
    vec3 se = texture(picture, at + vec2(1.0, 1.0) * inv).rgb;
    vec3 m = texture(picture, at).rgb;
    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);
    float lo = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float hi = max(lm, max(max(lnw, lne), max(lsw, lse)));
    // Only edges: where the neighbourhood's contrast is low the pixel stays as drawn (FXAA's edge thresholds).
    if (hi - lo < max(0.0625, hi * 0.166)) {
        colour = vec4(m, 1.0);
        return;
    }
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (0.25 * 0.125), 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * inv;
    vec3 a = 0.5 * (texture(picture, at + dir * (1.0 / 3.0 - 0.5)).rgb + texture(picture, at + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(picture, at + dir * -0.5).rgb + texture(picture, at + dir * 0.5).rgb);
    float lb = luma(b);
    colour = vec4((lb < lo || lb > hi) ? a : b, 1.0);
}
";

impl Fxaa {
    unsafe fn new(gl: &glow::Context) -> Result<Fxaa, String> {
        unsafe {
            let program = gl.create_program()?;
            let mut shaders = Vec::new();
            for (kind, source) in [(glow::VERTEX_SHADER, FXAA_VERTEX), (glow::FRAGMENT_SHADER, FXAA_FRAGMENT)] {
                let shader = gl.create_shader(kind)?;
                gl.shader_source(shader, source);
                gl.compile_shader(shader);
                if !gl.get_shader_compile_status(shader) {
                    return Err(gl.get_shader_info_log(shader));
                }
                gl.attach_shader(program, shader);
                shaders.push(shader);
            }
            gl.link_program(program);
            for shader in shaders {
                gl.detach_shader(program, shader);
                gl.delete_shader(shader);
            }
            if !gl.get_program_link_status(program) {
                return Err(gl.get_program_info_log(program));
            }
            let picture = gl.get_uniform_location(program, "picture");
            let inv = gl.get_uniform_location(program, "inv");
            let old = gl.get_parameter_i32(glow::CURRENT_PROGRAM);
            gl.use_program(Some(program));
            gl.uniform_1_i32(picture.as_ref(), 0);
            gl.use_program(std::num::NonZeroU32::new(old as u32).map(glow::NativeProgram));
            let vao = gl.create_vertex_array()?;
            let sampler = gl.create_sampler()?;
            gl.sampler_parameter_i32(sampler, glow::TEXTURE_MIN_FILTER, glow::LINEAR as i32);
            gl.sampler_parameter_i32(sampler, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
            gl.sampler_parameter_i32(sampler, glow::TEXTURE_WRAP_S, glow::CLAMP_TO_EDGE as i32);
            gl.sampler_parameter_i32(sampler, glow::TEXTURE_WRAP_T, glow::CLAMP_TO_EDGE as i32);
            Ok(Fxaa { program, vao, sampler, inv })
        }
    }

    /// The pass into the draw framebuffer bound now, every piece of state it touches put back after.
    unsafe fn draw(&self, gl: &glow::Context, texture: glow::Texture, w: i32, h: i32) {
        unsafe {
            let program = gl.get_parameter_i32(glow::CURRENT_PROGRAM);
            let vao = gl.get_parameter_i32(glow::VERTEX_ARRAY_BINDING);
            let unit = gl.get_parameter_i32(glow::ACTIVE_TEXTURE);
            gl.active_texture(glow::TEXTURE0);
            let bound = gl.get_parameter_i32(glow::TEXTURE_BINDING_2D);
            let sampler = gl.get_parameter_i32(glow::SAMPLER_BINDING);
            let mut viewport = [0i32; 4];
            gl.get_parameter_i32_slice(glow::VIEWPORT, &mut viewport);
            let enabled: Vec<(u32, bool)> = [glow::BLEND, glow::DEPTH_TEST, glow::STENCIL_TEST, glow::CULL_FACE]
                .into_iter()
                .map(|cap| (cap, gl.is_enabled(cap)))
                .collect();
            for (cap, _) in &enabled {
                gl.disable(*cap);
            }

            gl.use_program(Some(self.program));
            gl.bind_vertex_array(Some(self.vao));
            gl.bind_texture(glow::TEXTURE_2D, Some(texture));
            gl.bind_sampler(0, Some(self.sampler));
            gl.viewport(0, 0, w, h);
            gl.uniform_2_f32(self.inv.as_ref(), 1.0 / w as f32, 1.0 / h as f32);
            gl.draw_arrays(glow::TRIANGLES, 0, 3);

            for (cap, on) in enabled {
                if on {
                    gl.enable(cap);
                }
            }
            gl.viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            gl.bind_sampler(0, std::num::NonZeroU32::new(sampler as u32).map(glow::NativeSampler));
            gl.bind_texture(glow::TEXTURE_2D, std::num::NonZeroU32::new(bound as u32).map(glow::NativeTexture));
            gl.active_texture(unit as u32);
            gl.bind_vertex_array(std::num::NonZeroU32::new(vao as u32).map(glow::NativeVertexArray));
            gl.use_program(std::num::NonZeroU32::new(program as u32).map(glow::NativeProgram));
        }
    }
}

impl Output {
    pub fn new(host: Host) -> Output {
        let gl = unsafe { glow::Context::from_loader_function(|name| host.proc(name)) };
        Output { host, gl, read: None, fxaa: None, no_fxaa: false }
    }

    /// The picture in `texture` (its top `w` x `h`) into the player's framebuffer, smoothed by FXAA when `smooth`,
    /// and the stick's pointer over it. False when the texture isn't a GL one.
    pub fn present(
        &mut self,
        texture: &wgpu::Texture,
        w: u32,
        h: u32,
        smooth: bool,
        drawn: (u32, u32),
        pointer: Option<(i64, i64)>,
    ) -> bool {
        // Drawn smaller (RGDS_FLASH_RES): scaled up by the blit, no FXAA.
        let scaled = drawn != (w, h);
        let smooth = smooth && !scaled;
        let raw = unsafe { texture.as_hal::<wgpu::hal::api::Gles>() }.and_then(|t| match t.inner {
            wgpu::hal::gles::TextureInner::Texture { raw, .. } => Some(raw),
            _ => None,
        });
        let Some(raw) = raw else { return false };
        if smooth && self.fxaa.is_none() && !self.no_fxaa {
            match unsafe { Fxaa::new(&self.gl) } {
                Ok(f) => self.fxaa = Some(f),
                Err(e) => {
                    tracing::warn!("flash: no FXAA ({e}); the picture is blitted as drawn");
                    self.no_fxaa = true;
                }
            }
        }
        let target = unsafe { (self.host.framebuffer)() };
        let gl = &self.gl;
        let (w, h) = (w as i32, h as i32);
        unsafe {
            let scissor = gl.is_enabled(glow::SCISSOR_TEST);
            gl.disable(glow::SCISSOR_TEST);
            gl.color_mask(true, true, true, true);
            match (&self.fxaa, smooth) {
                (Some(fxaa), true) => {
                    gl.bind_framebuffer(glow::DRAW_FRAMEBUFFER, framebuffer(target));
                    fxaa.draw(gl, raw, w, h);
                }
                _ => {
                    if self.read.is_none() {
                        self.read = gl.create_framebuffer().ok();
                    }
                    let Some(read) = self.read else { return false };
                    gl.bind_framebuffer(glow::READ_FRAMEBUFFER, Some(read));
                    gl.framebuffer_texture_2d(
                        glow::READ_FRAMEBUFFER,
                        glow::COLOR_ATTACHMENT0,
                        glow::TEXTURE_2D,
                        Some(raw),
                        0,
                    );
                    gl.bind_framebuffer(glow::DRAW_FRAMEBUFFER, framebuffer(target));
                    let filter = if scaled { glow::LINEAR } else { glow::NEAREST };
                    let (dw, dh) = (drawn.0 as i32, drawn.1 as i32);
                    gl.blit_framebuffer(0, 0, dw, dh, 0, 0, w, h, glow::COLOR_BUFFER_BIT, filter);
                    gl.framebuffer_texture_2d(glow::READ_FRAMEBUFFER, glow::COLOR_ATTACHMENT0, glow::TEXTURE_2D, None, 0);
                    gl.bind_framebuffer(glow::READ_FRAMEBUFFER, None);
                }
            }
            if let Some(at) = pointer {
                // The arrow as runs of one colour, each a scissored clear: no shader, no texture.
                gl.enable(glow::SCISSOR_TEST);
                for (x, y, len, colour) in pointer_runs(at, w, h) {
                    let c = |shift: u32| ((colour >> shift) & 0xFF) as f32 / 255.0;
                    gl.scissor(x, y, len, POINTER_SCALE);
                    gl.clear_color(c(16), c(8), c(0), 1.0);
                    gl.clear(glow::COLOR_BUFFER_BIT);
                }
                gl.disable(glow::SCISSOR_TEST);
            }
            gl.bind_framebuffer(glow::DRAW_FRAMEBUFFER, framebuffer(target));
            if scissor {
                gl.enable(glow::SCISSOR_TEST);
            }
        }
        true
    }
}

impl Drop for Output {
    fn drop(&mut self) {
        unsafe {
            if let Some(read) = self.read.take() {
                self.gl.delete_framebuffer(read);
            }
            if let Some(f) = self.fxaa.take() {
                self.gl.delete_program(f.program);
                self.gl.delete_vertex_array(f.vao);
                self.gl.delete_sampler(f.sampler);
            }
        }
    }
}

fn framebuffer(name: usize) -> Option<glow::Framebuffer> {
    std::num::NonZeroU32::new(name as u32).map(glow::NativeFramebuffer)
}

pub const POINTER_SCALE: i32 = 2;

/// The arrow (crate::ARROW) at `at` as (x, y, length, colour) runs inside a w x h picture.
pub fn pointer_runs(at: (i64, i64), w: i32, h: i32) -> Vec<(i32, i32, i32, u32)> {
    let mut runs = Vec::new();
    for (row, line) in crate::ARROW.iter().enumerate() {
        let bytes = line.as_bytes();
        let mut col = 0;
        while col < bytes.len() {
            let c = bytes[col];
            let start = col;
            while col < bytes.len() && bytes[col] == c {
                col += 1;
            }
            let colour = match c {
                b'X' => 0x202020,
                b'o' => 0xFFFFFF,
                _ => continue,
            };
            let x0 = (at.0 + (start as i64) * POINTER_SCALE as i64).max(0);
            let x1 = (at.0 + (col as i64) * POINTER_SCALE as i64).min(w as i64);
            let y = at.1 + row as i64 * POINTER_SCALE as i64;
            if x1 > x0 && y >= 0 && y + (POINTER_SCALE as i64) <= h as i64 {
                runs.push((x0 as i32, y as i32, (x1 - x0) as i32, colour));
            }
        }
    }
    runs
}
