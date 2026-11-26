//! Runs a Flash game through the core's libretro entry points with no screen, no sound and no input: to reproduce a
//! crash or time a game on the handheld without taking its panels.
//!
//!     headless GAME.swf|GAME.zip [FRAMES] [SAVE_DIR]
//!
//! On Linux it offers the core a GLES 3 context of its own (EGL with no window, Mesa's surfaceless platform) and a
//! framebuffer as big as the core's largest picture (1024x768 on a Plus), as RG DS Play does, so the core draws there; RGDS_FLASH_SOFTWARE=1 refuses it (the read-back
//! path). HEADLESS_READ=1 reads every picture with glReadPixels, as Play does for a game on the lower panel;
//! HEADLESS_SHOT=FILE.ppm writes the last picture. Prints the frames run, the pictures sent and the time a frame took
//! (mean and worst); RGDS_FLASH_LOG works as in the player.
//!
//! Scripted play, to time a game past its title: HEADLESS_TAPS="FRAME:X,Y;..." touches the picture at X, Y (0-1 of its
//! width and height) for 4 frames from FRAME; HEADLESS_DRAGS="FROM-TO:X,Y>X2,Y2;..." keeps a finger down from frame
//! FROM until TO, sliding from the first place to the second (a held mouse button); HEADLESS_HOLD="FRAME:ID;..." holds
//! joypad button ID (libretro's, 4 = up) from FRAME on, "FROM-TO:ID" until TO; HEADLESS_SHOTS="FRAME,..." writes
//! shot-FRAME.ppm (with HEADLESS_SHOT's folder, or the current one).

use std::ffi::{CString, c_uint, c_void};
use std::os::raw::c_char;
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use ruffle_libretro::libretro as lr;
use ruffle_libretro::*;

static FRAME: AtomicUsize = AtomicUsize::new(0);
/// A finger on the picture: from a frame until another, sliding from one place to a second.
static TAPS: std::sync::OnceLock<Vec<(usize, usize, (f64, f64), (f64, f64))>> = std::sync::OnceLock::new();
static HOLDS: std::sync::OnceLock<Vec<(usize, usize, u32)>> = std::sync::OnceLock::new();
static PICTURES: AtomicUsize = AtomicUsize::new(0);
static SAVE: std::sync::OnceLock<CString> = std::sync::OnceLock::new();
static RESET: Mutex<Option<lr::HwContextResetFn>> = Mutex::new(None);
/// The last picture, R, G, B rows from the top.
static LAST: Mutex<(u32, u32, Vec<u8>)> = Mutex::new((0, 0, Vec::new()));

#[cfg(target_os = "linux")]
mod context {
    //! The GLES context: EGL on Mesa's surfaceless platform, one framebuffer as RG DS Play makes it.
    use glow::HasContext;
    use std::ffi::{CStr, c_char, c_void};
    use std::sync::OnceLock;

    pub struct Context {
        pub gl: glow::Context,
        pub fbo: u32,
    }
    unsafe impl Send for Context {}
    unsafe impl Sync for Context {}

    static EGL: OnceLock<khronos_egl::DynamicInstance<khronos_egl::EGL1_5>> = OnceLock::new();
    pub static CONTEXT: OnceLock<Context> = OnceLock::new();

    pub fn make() -> Result<(), String> {
        let lib = unsafe { libloading::Library::new("libEGL.so.1") }.map_err(|e| e.to_string())?;
        let egl = unsafe { khronos_egl::DynamicInstance::<khronos_egl::EGL1_5>::load_required_from(lib) }
            .map_err(|e| format!("{e:?}"))?;
        const PLATFORM_SURFACELESS_MESA: khronos_egl::Enum = 0x31DD;
        let display = unsafe {
            egl.get_platform_display(
                PLATFORM_SURFACELESS_MESA,
                khronos_egl::DEFAULT_DISPLAY,
                &[khronos_egl::ATTRIB_NONE],
            )
        }
        .map_err(|e| format!("{e:?}"))?;
        egl.initialize(display).map_err(|e| format!("{e:?}"))?;
        egl.bind_api(khronos_egl::OPENGL_ES_API).map_err(|e| format!("{e:?}"))?;
        let config = egl
            .choose_first_config(
                display,
                &[
                    khronos_egl::RENDERABLE_TYPE,
                    khronos_egl::OPENGL_ES3_BIT,
                    khronos_egl::SURFACE_TYPE,
                    0,
                    khronos_egl::NONE,
                ],
            )
            .map_err(|e| format!("{e:?}"))?
            .ok_or("no EGL config")?;
        let context = egl
            .create_context(display, config, None, &[khronos_egl::CONTEXT_MAJOR_VERSION, 3, khronos_egl::NONE])
            .map_err(|e| format!("{e:?}"))?;
        egl.make_current(display, None, None, Some(context)).map_err(|e| format!("{e:?}"))?;
        let egl = EGL.get_or_init(|| egl);
        let gl = unsafe {
            glow::Context::from_loader_function(|name| {
                egl.get_proc_address(name).map_or(std::ptr::null(), |f| f as *const c_void)
            })
        };
        let fbo = unsafe {
            let fbo = gl.create_framebuffer()?;
            gl.bind_framebuffer(glow::FRAMEBUFFER, Some(fbo));
            let colour = gl.create_renderbuffer()?;
            gl.bind_renderbuffer(glow::RENDERBUFFER, Some(colour));
            let (w, h) = ruffle_libretro::max_picture();
            gl.renderbuffer_storage(glow::RENDERBUFFER, glow::RGBA8, w as i32, h as i32);
            gl.framebuffer_renderbuffer(glow::FRAMEBUFFER, glow::COLOR_ATTACHMENT0, glow::RENDERBUFFER, Some(colour));
            fbo.0.get()
        };
        let _ = CONTEXT.set(Context { gl, fbo });
        Ok(())
    }

    pub unsafe extern "C" fn framebuffer() -> usize {
        CONTEXT.get().map_or(0, |c| c.fbo as usize)
    }
    pub unsafe extern "C" fn proc_address(sym: *const c_char) -> Option<ruffle_libretro::libretro::ProcAddressFn> {
        let name = unsafe { CStr::from_ptr(sym) }.to_str().ok()?;
        EGL.get()?.get_proc_address(name).map(|f| unsafe { std::mem::transmute(f) })
    }

    /// HEADLESS_WAIT=1: wait for the GPU to finish the picture without copying it (glFinish), the time it waited.
    pub fn finish() -> std::time::Duration {
        let t = std::time::Instant::now();
        if let Some(c) = CONTEXT.get() {
            unsafe { c.gl.finish() };
        }
        t.elapsed()
    }

    /// HEADLESS_WAIT=lag: wait for the previous picture's GPU work only (a fence), as a swap chain one picture deep
    /// would, so the GPU draws one picture while the CPU runs the next tick. The time it waited.
    pub fn finish_previous() -> std::time::Duration {
        static PREVIOUS: std::sync::Mutex<usize> = std::sync::Mutex::new(0);
        let t = std::time::Instant::now();
        if let Some(c) = CONTEXT.get() {
            let mut previous = PREVIOUS.lock().unwrap();
            unsafe {
                if *previous != 0 {
                    let fence: glow::NativeFence = std::mem::transmute(*previous);
                    c.gl.client_wait_sync(fence, glow::SYNC_FLUSH_COMMANDS_BIT, 1_000_000_000);
                    c.gl.delete_sync(fence);
                }
                *previous = c.gl.fence_sync(glow::SYNC_GPU_COMMANDS_COMPLETE, 0).map_or(0, |f| std::mem::transmute(f));
                c.gl.flush();
            }
        }
        t.elapsed()
    }

    /// The picture the core blitted into the framebuffer, R, G, B rows from the top.
    pub fn read(w: u32, h: u32) -> Vec<u8> {
        let Some(c) = CONTEXT.get() else { return Vec::new() };
        let mut rgba = vec![0u8; (w * h * 4) as usize];
        unsafe {
            c.gl.bind_framebuffer(
                glow::READ_FRAMEBUFFER,
                std::num::NonZeroU32::new(c.fbo).map(glow::NativeFramebuffer),
            );
            c.gl.read_pixels(
                0,
                0,
                w as i32,
                h as i32,
                glow::RGBA,
                glow::UNSIGNED_BYTE,
                glow::PixelPackData::Slice(Some(&mut rgba)),
            );
        }
        rgba.as_chunks::<4>().0.iter().flat_map(|p| [p[0], p[1], p[2]]).collect()
    }
}

unsafe extern "C" fn environment(cmd: c_uint, data: *mut c_void) -> bool {
    match cmd {
        lr::ENV_SET_PIXEL_FORMAT | lr::ENV_SET_VARIABLES | lr::ENV_SET_KEYBOARD_CALLBACK => true,
        lr::ENV_GET_SAVE_DIRECTORY => {
            let dir = SAVE.get().unwrap();
            unsafe { *(data as *mut *const c_char) = dir.as_ptr() };
            true
        }
        #[cfg(target_os = "linux")]
        lr::ENV_SET_HW_RENDER => {
            if std::env::var_os("RGDS_FLASH_SOFTWARE").is_some() {
                return false;
            }
            if let Err(e) = context::make() {
                eprintln!("headless: no GLES context: {e}");
                return false;
            }
            let hw = unsafe { &mut *(data as *mut lr::HwRenderCallback) };
            hw.get_current_framebuffer = Some(context::framebuffer);
            hw.get_proc_address = Some(context::proc_address);
            *RESET.lock().unwrap() = hw.context_reset;
            true
        }
        _ => false,
    }
}

static READ_EACH: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
static WAITED_US: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
static WAITED_WORST_US: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

unsafe extern "C" fn video(data: *const c_void, w: c_uint, h: c_uint, pitch: usize) {
    if data.is_null() {
        return;
    }
    PICTURES.fetch_add(1, Ordering::Relaxed);
    #[cfg_attr(not(target_os = "linux"), allow(unused_variables))]
    let keep = *READ_EACH.get_or_init(|| std::env::var_os("HEADLESS_READ").is_some());
    if data == lr::HW_FRAME_BUFFER_VALID {
        #[cfg(target_os = "linux")]
        if let Some(how) = std::env::var_os("HEADLESS_WAIT") {
            let waited = if how == "lag" { context::finish_previous() } else { context::finish() };
            let us = waited.as_micros() as u64;
            WAITED_US.fetch_add(us, Ordering::Relaxed);
            WAITED_WORST_US.fetch_max(us, Ordering::Relaxed);
        }
        // HEADLESS_READ_UNTIL=N: read back every picture until frame N only, the timing scripted taps were made for
        // (loading runs on wall-clock time), with the frames after it measured clean.
        #[cfg(target_os = "linux")]
        {
            let until =
                std::env::var("HEADLESS_READ_UNTIL").ok().and_then(|v| v.parse::<usize>().ok()).unwrap_or(0);
            if keep
                || FRAME.load(Ordering::Relaxed) < until
                || std::env::var_os("HEADLESS_SHOT").is_some()
                || std::env::var_os("HEADLESS_SHOTS").is_some()
            {
                *LAST.lock().unwrap() = (w, h, context::read(w, h));
            }
        }
        return;
    }
    let rows = unsafe { std::slice::from_raw_parts(data as *const u8, pitch * h as usize) };
    let rgb = rows
        .chunks(pitch)
        .flat_map(|row| {
            row[..w as usize * 4].as_chunks::<4>().0.iter().flat_map(|p| [p[2], p[1], p[0]]).collect::<Vec<_>>()
        })
        .collect();
    *LAST.lock().unwrap() = (w, h, rgb);
}
unsafe extern "C" fn audio(_data: *const i16, frames: usize) -> usize {
    frames
}
unsafe extern "C" fn poll() {}
unsafe extern "C" fn state(_port: c_uint, device: c_uint, _index: c_uint, id: c_uint) -> i16 {
    let frame = FRAME.load(Ordering::Relaxed);
    match device {
        lr::DEVICE_POINTER => {
            let tap = TAPS.get().and_then(|t| t.iter().find(|(from, to, _, _)| frame >= *from && frame < *to));
            let Some(&(from, to, a, b)) = tap else { return if id == lr::POINTER_PRESSED { 0 } else { -0x8000 } };
            let along = (frame - from) as f64 / (to - from).saturating_sub(1).max(1) as f64;
            let (x, y) = (a.0 + (b.0 - a.0) * along, a.1 + (b.1 - a.1) * along);
            match id {
                lr::POINTER_X => (x * 65535.0 - 32767.0) as i16,
                lr::POINTER_Y => (y * 65535.0 - 32767.0) as i16,
                _ => 1,
            }
        }
        lr::DEVICE_JOYPAD => {
            HOLDS.get().is_some_and(|h| h.iter().any(|&(from, to, b)| frame >= from && frame < to && b == id)) as i16
        }
        _ => 0,
    }
}

fn script<T>(name: &str, item: impl Fn(&str, &str) -> Option<T>) -> Vec<T> {
    let text = std::env::var(name).unwrap_or_default();
    text.split(';').filter_map(|e| e.split_once(':').and_then(|(f, rest)| item(f.trim(), rest.trim()))).collect()
}

fn write_shot(path: &std::path::Path) {
    let (w, h, rgb) = LAST.lock().unwrap().clone();
    let mut ppm = format!("P6\n{w} {h}\n255\n").into_bytes();
    ppm.extend_from_slice(&rgb);
    std::fs::write(path, ppm).expect("the shot");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let Some(game) = args.get(1) else {
        eprintln!("usage: headless GAME.swf|GAME.zip [FRAMES] [SAVE_DIR]");
        std::process::exit(2);
    };
    let place = |xy: &str| {
        let (x, y) = xy.split_once(',')?;
        Some((x.trim().parse::<f64>().ok()?, y.trim().parse::<f64>().ok()?))
    };
    let span = |f: &str| match f.split_once('-') {
        Some((from, to)) => Some((from.trim().parse::<usize>().ok()?, to.trim().parse::<usize>().ok()?)),
        None => Some((f.parse::<usize>().ok()?, usize::MAX)),
    };
    let mut touches = script("HEADLESS_TAPS", |f, xy| {
        let (at, xy) = (f.parse::<usize>().ok()?, place(xy)?);
        Some((at, at + 4, xy, xy))
    });
    touches.extend(script("HEADLESS_DRAGS", |f, path| {
        let (from, to) = span(f)?;
        let (a, b) = path.split_once('>').unwrap_or((path, path));
        Some((from, to, place(a)?, place(b)?))
    }));
    TAPS.set(touches).unwrap();
    HOLDS
        .set(script("HEADLESS_HOLD", |f, id| {
            let (from, to) = span(f)?;
            Some((from, to, id.parse().ok()?))
        }))
        .unwrap();
    let shots: Vec<usize> = std::env::var("HEADLESS_SHOTS")
        .unwrap_or_default()
        .split(',')
        .filter_map(|f| f.trim().parse().ok())
        .collect();
    let shot_dir = std::env::var_os("HEADLESS_SHOT")
        .map(std::path::PathBuf::from)
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_default();
    let frames: u32 = args.get(2).and_then(|f| f.parse().ok()).unwrap_or(600);
    let save =
        args.get(3).cloned().unwrap_or_else(|| std::env::temp_dir().join("flash-headless").display().to_string());
    let _ = std::fs::create_dir_all(&save);
    SAVE.set(CString::new(save).unwrap()).unwrap();

    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio);
    retro_set_input_poll(poll);
    retro_set_input_state(state);
    retro_init();
    let path = CString::new(game.as_str()).unwrap();
    let info = lr::GameInfo { path: path.as_ptr(), data: std::ptr::null(), size: 0, meta: std::ptr::null() };
    if !retro_load_game(&info) {
        eprintln!("headless: the core refused {game}");
        std::process::exit(1);
    }
    // The player's first frame makes its context and tells the core.
    let hw = RESET.lock().unwrap().take();
    if let Some(reset) = hw {
        unsafe { reset() };
    }
    println!("headless: {}", if hw.is_some() { "on a GLES context of its own" } else { "pictures read back" });
    let (mut total, mut worst, mut over) = (0.0f64, 0.0f64, [0u32; 3]);
    let mut window = (0.0f64, 0.0f64);
    for frame in 0..frames as usize {
        FRAME.store(frame, Ordering::Relaxed);
        let at = Instant::now();
        retro_run();
        let ms = at.elapsed().as_secs_f64() * 1e3;
        total += ms;
        worst = worst.max(ms);
        for (i, limit) in [16.7, 33.3, 66.7].iter().enumerate() {
            over[i] += (ms > *limit) as u32;
        }
        window = (window.0 + ms, window.1.max(ms));
        if (frame + 1) % 300 == 0 {
            println!("headless: frames {}-{}: {:.2} ms mean, {:.2} worst", frame - 298, frame + 1, window.0 / 300.0, window.1);
            window = (0.0, 0.0);
            let (w, worst) = (WAITED_US.swap(0, Ordering::Relaxed), WAITED_WORST_US.swap(0, Ordering::Relaxed));
            if w > 0 {
                println!("headless:   GPU wait {:.2} ms a frame, worst {:.2}", w as f64 / 300e3, worst as f64 / 1e3);
            }
        }
        if shots.contains(&frame) {
            write_shot(&shot_dir.join(format!("shot-{frame}.ppm")));
        }
    }
    if let Some(out) = std::env::var_os("HEADLESS_SHOT") {
        write_shot(std::path::Path::new(&out));
    }
    retro_unload_game();
    println!(
        "headless: {frames} frames, {} pictures, {:.2} ms a frame (worst {:.2}); over 16.7/33.3/66.7 ms: {:?}",
        PICTURES.load(Ordering::Relaxed),
        total / frames.max(1) as f64,
        worst,
        over
    );
}
