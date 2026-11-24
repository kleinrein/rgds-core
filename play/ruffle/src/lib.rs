//! Flash games in RG DS Play: Ruffle (https://ruffle.rs) behind the libretro interface.
//!
//! The movie is drawn by wgpu into a texture of its own (Vulkan or GLES on the handheld, Metal on a Mac) and read
//! back each shown frame, so the player gets a plain XRGB8888 picture and treats Flash like any other system: the
//! game menu, screenshots, shaders, frames, playtime.
//!
//! Input: a finger on the picture is the mouse (RETRO_DEVICE_POINTER). "Direct" touch presses where the finger
//! lands; "Hover" moves the pointer with the finger and clicks on a tap, for games that aim with the mouse. The
//! left stick moves a drawn pointer and R clicks, for games that need hover without a finger. The pad's buttons
//! are keys chosen by option (`ruffle_btn_*`), the on-screen keyboard types through the keyboard callback.
//!
//! Flash keeps its own saves (SharedObjects), written under the save folder; there are no save states.

// libretro hands its structs over as raw pointers to safe-looking C entry points.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

#[cfg(feature = "mimalloc")]
#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

mod content;
#[cfg(target_os = "linux")]
mod gl;
#[cfg(not(target_os = "linux"))]
mod gl {
    //! No GLES context from the player off Linux (a Mac's Play has none): the picture is always read back.
    #[derive(Clone, Copy)]
    pub struct Host;
    pub struct Output;
    impl Output {
        pub fn present(
            &mut self,
            _t: &ruffle_render_wgpu::wgpu::Texture,
            _w: u32,
            _h: u32,
            _smooth: bool,
            _drawn: (u32, u32),
            _p: Option<(i64, i64)>,
        ) -> bool {
            false
        }
    }
}
mod keys;
pub mod libretro;
mod navigator;
mod storage;
mod target;
mod ui;

use std::ffi::{CStr, CString, c_char, c_uint, c_void};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use ruffle_core::backend::audio::{
    AudioBackend, AudioMixer, AudioMixerProxy, DecodeError, RegisterError, SoundHandle, SoundInstanceHandle,
    SoundStreamInfo, SoundTransform, swf,
};
use ruffle_core::backend::navigator::NullExecutor;
use ruffle_core::config::Letterbox;
use ruffle_core::events::{KeyDescriptor, MouseButton, PlayerEvent};
use ruffle_core::{FloatDuration, Player, PlayerBuilder, impl_audio_mixer_backend};
use ruffle_render::quality::StageQuality;
use ruffle_render_wgpu::backend::{WgpuRenderBackend, create_wgpu_instance, request_adapter_and_device};
use ruffle_render_wgpu::descriptors::Descriptors;
use ruffle_render_wgpu::wgpu;

use crate::content::Content;
use crate::keys::Bind;
use crate::libretro as lr;
use crate::target::FlashTarget;

const SAMPLE_RATE: u32 = 44100;
const FPS: f64 = 60.0;
/// The picture never exceeds a panel (640x480), drawn at that size so vector art stays sharp.
const MAX_W: u32 = 640;
const MAX_H: u32 = 480;

// ---- Callbacks (set before anything else, kept as plain addresses so every thread sees them) ----

static ENV: AtomicUsize = AtomicUsize::new(0);
static VIDEO: AtomicUsize = AtomicUsize::new(0);
static AUDIO_BATCH: AtomicUsize = AtomicUsize::new(0);
static POLL: AtomicUsize = AtomicUsize::new(0);
static INPUT: AtomicUsize = AtomicUsize::new(0);

fn env(cmd: c_uint, data: *mut c_void) -> bool {
    let f = ENV.load(Ordering::Acquire);
    if f == 0 {
        return false;
    }
    let f: lr::EnvironmentFn = unsafe { std::mem::transmute(f) };
    unsafe { f(cmd, data) }
}

fn input(port: c_uint, device: c_uint, index: c_uint, id: c_uint) -> i16 {
    let f = INPUT.load(Ordering::Acquire);
    if f == 0 {
        return 0;
    }
    let f: lr::InputStateFn = unsafe { std::mem::transmute(f) };
    unsafe { f(port, device, index, id) }
}

fn variable(key: &str) -> Option<String> {
    let key = CString::new(key).ok()?;
    let mut v = lr::Variable { key: key.as_ptr(), value: std::ptr::null() };
    if !env(lr::ENV_GET_VARIABLE, &mut v as *mut _ as *mut c_void) || v.value.is_null() {
        return None;
    }
    Some(unsafe { CStr::from_ptr(v.value) }.to_string_lossy().into_owned())
}

fn env_dir(cmd: c_uint) -> Option<PathBuf> {
    let mut p: *const c_char = std::ptr::null();
    if !env(cmd, &mut p as *mut _ as *mut c_void) || p.is_null() {
        return None;
    }
    Some(PathBuf::from(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()))
}

// ---- Audio: Ruffle's own mixer, pulled once a frame ----

struct PulledAudio {
    mixer: AudioMixer,
}

impl AudioBackend for PulledAudio {
    impl_audio_mixer_backend!(mixer);
    fn play(&mut self) {}
    fn pause(&mut self) {}
}

// ---- Keys typed on the on-screen keyboard arrive outside retro_run ----

static TYPED: Mutex<Vec<(bool, u32, u32)>> = Mutex::new(Vec::new());

unsafe extern "C" fn keyboard_event(down: bool, keycode: c_uint, character: u32, _mods: u16) {
    if let Ok(mut q) = TYPED.lock() {
        q.push((down, keycode, character));
    }
}

// ---- The running game ----

/// What the left stick does: moves a drawn pointer, or presses four keys (driving and platform games).
#[derive(Clone, Copy)]
enum Stick {
    Pointer,
    /// Up, down, left, right.
    Keys([KeyDescriptor; 4]),
}

/// Flashpoint's tags for games steered with the keys, not the mouse: the stick is arrows for those by default.
const KEY_GENRES: [&str; 10] =
    ["racing", "driving", "platform", "fighting", "beat 'em up", "shoot 'em up", "runner", "maze", "snake", "flying"];

fn stick_of(choice: &str, details: &std::collections::HashMap<String, String>) -> Stick {
    let keys = |names: [&str; 4]| match names.map(keys::bind_named) {
        [Bind::Key(u), Bind::Key(d), Bind::Key(l), Bind::Key(r)] => Stick::Keys([u, d, l, r]),
        _ => Stick::Pointer,
    };
    // The game's own line first (`stick=arrows` in its .flashpoint), then the option.
    let choice = details.get("stick").map(String::as_str).unwrap_or(choice);
    match choice {
        "arrows" => keys(["up", "down", "left", "right"]),
        "wasd" => keys(["w", "s", "a", "d"]),
        "pointer" => Stick::Pointer,
        _ if tagged(details, &KEY_GENRES) => keys(["up", "down", "left", "right"]),
        _ => Stick::Pointer,
    }
}

/// Whether Flashpoint's genre or tags for the game name any of these.
fn tagged(details: &std::collections::HashMap<String, String>, genres: &[&str]) -> bool {
    let tags = format!(
        "{} {}",
        details.get("genre").map(String::as_str).unwrap_or(""),
        details.get("tags").map(String::as_str).unwrap_or("")
    )
    .to_lowercase();
    genres.iter().any(|g| tags.contains(g))
}

/// A button's key for this game: its own line in the .flashpoint, else the option when it was changed, else the
/// default for its kind of game (a racing game drives on A and brakes on B, its arrows' up and down).
fn button_choice(
    key: &str,
    default: &str,
    details: &std::collections::HashMap<String, String>,
    set: Option<String>,
) -> String {
    if let Some(own) = details.get(key.trim_start_matches("ruffle_")) {
        return own.clone();
    }
    if let Some(v) = set.filter(|v| v != default) {
        return v;
    }
    let racing = tagged(details, &["racing", "driving"]);
    match key {
        "ruffle_btn_a" if racing => "up".into(),
        "ruffle_btn_b" if racing => "down".into(),
        _ => default.into(),
    }
}

/// Who can hold a mouse button down (Game::button): a finger on the picture, a tap's click, the second screen's
/// touch pad, and from HELD_PAD_BUTTON up a bit for each of the pad's buttons.
const HELD_FINGER: u32 = 1;
const HELD_TAP: u32 = 2;
const HELD_TOUCH_PAD: u32 = 4;
const HELD_PAD_BUTTON: u32 = 8;

#[derive(Clone, Copy, PartialEq)]
enum Touch {
    Direct,
    Hover,
}

/// Pad buttons the core maps, in the order of their option keys.
const BUTTONS: [(c_uint, &str, &str); 14] = [
    (lr::JOY_A, "ruffle_btn_a", "space"),
    (lr::JOY_B, "ruffle_btn_b", "z"),
    (lr::JOY_X, "ruffle_btn_x", "x"),
    (lr::JOY_Y, "ruffle_btn_y", "c"),
    // Either shoulder is the mouse's button: whichever hand is not on the touch pad or the stick holds it.
    (lr::JOY_L, "ruffle_btn_l", "mouse"),
    (lr::JOY_R, "ruffle_btn_r", "mouse"),
    (lr::JOY_L2, "ruffle_btn_l2", "shift"),
    (lr::JOY_R2, "ruffle_btn_r2", "right click"),
    (lr::JOY_START, "ruffle_btn_start", "enter"),
    (lr::JOY_SELECT, "ruffle_btn_select", "p"),
    (lr::JOY_UP, "ruffle_dpad_up", "up"),
    (lr::JOY_DOWN, "ruffle_dpad_down", "down"),
    (lr::JOY_LEFT, "ruffle_dpad_left", "left"),
    (lr::JOY_RIGHT, "ruffle_dpad_right", "right"),
];

struct Game {
    path: PathBuf,
    player: Arc<Mutex<Player>>,
    executor: NullExecutor,
    mixer: AudioMixerProxy,
    shared: Arc<ui::Shared>,
    width: u32,
    height: u32,
    /// Anti-aliased by FXAA as the picture goes to the player (gl.rs), the renderer itself never multisampled: on the
    /// RG DS's Panfrost wgpu's GL MSAA made a HappyWheels level 30 ms of drawing a frame, none 1.1 ms.
    smooth: bool,
    /// The size Ruffle draws at, the picture's own unless RGDS_FLASH_RES (a percentage, measuring) shrinks it; the
    /// blit scales it up.
    drawn: (u32, u32),
    /// Frame skip: when the last call came, how far behind real time the movie is (seconds), and pictures left out
    /// in a row. The movie still advances every call, so a game too heavy to draw every picture keeps its speed.
    last_call: Option<Instant>,
    behind: f64,
    skipped: u32,
    /// What a call's movie and a picture's drawing have cost lately (ms, smoothed): how many pictures being late
    /// may leave out (`skips_allowed`).
    movie_ms: f64,
    draw_ms: f64,
    /// The last picture read from the GPU; `frame` is it with the stick's pointer drawn over.
    clean: Vec<u32>,
    frame: Vec<u32>,
    /// Where the pointer is drawn in `frame` now (None: not drawn), so moving it needs no new render.
    drawn_pointer: Option<(i64, i64)>,
    stick: Stick,
    stick_held: [bool; 4],
    binds: [Bind; 14],
    held: [bool; 14],
    touch: Touch,
    finger: Option<(f64, f64)>,
    finger_start: Option<(f64, f64, u32)>,
    finger_moved: bool,
    cursor: (f64, f64),
    /// Frames left that the drawn pointer stays up after the stick last moved it.
    cursor_shown: u32,
    cursor_speed: f64,
    /// What holds each mouse button down, a bit a holder (`HELD_*`, a pad button's own bit): the button is down
    /// while any does, so a shoulder button held through a tap on the touch pad stays held.
    left_held: u32,
    right_held: u32,
    /// Frames left of a tap's click (Hover): the button stays down long enough for a game that reads it each frame.
    tap_frames: u32,
    /// The player's framebuffer, when Ruffle draws on its GLES context: the picture is blitted there, never read.
    out: Option<gl::Output>,
    frames: u32,
    /// Where the time went since the last report (RGDS_FLASH_LOG=debug).
    spent: Spent,
    audio: Vec<i16>,
}

struct Slot(Option<Game>);
// libretro calls a core from one thread at a time; the lock below is what keeps that true.
unsafe impl Send for Slot {}
static GAME: Mutex<Slot> = Mutex::new(Slot(None));

fn fit(stage_w: f64, stage_h: f64) -> (u32, u32) {
    let (sw, sh) = (stage_w.max(1.0), stage_h.max(1.0));
    let scale = (MAX_W as f64 / sw).min(MAX_H as f64 / sh);
    let w = ((sw * scale).round() as u32).clamp(16, MAX_W) & !1;
    let h = ((sh * scale).round() as u32).clamp(16, MAX_H) & !1;
    (w, h)
}

fn backends() -> wgpu::Backends {
    match std::env::var("RGDS_FLASH_GPU").as_deref() {
        Ok("gl") | Ok("gles") => wgpu::Backends::GL,
        Ok("vulkan") => wgpu::Backends::VULKAN,
        _ if cfg!(target_os = "macos") => wgpu::Backends::METAL,
        _ => wgpu::Backends::VULKAN | wgpu::Backends::GL,
    }
}

fn gpu() -> Result<Arc<Descriptors>, String> {
    let wanted = backends();
    let mut last = String::new();
    // Vulkan first (PanVK on the handheld), then GLES through EGL with no window.
    for b in [wgpu::Backends::VULKAN, wgpu::Backends::METAL, wgpu::Backends::GL] {
        if !wanted.contains(b) {
            continue;
        }
        let instance = create_wgpu_instance(b, wgpu::BackendOptions::default(), None);
        match futures::executor::block_on(request_adapter_and_device(
            b,
            &instance,
            None,
            wgpu::PowerPreference::HighPerformance,
        )) {
            Ok((adapter, device, queue)) => {
                tracing::info!("flash: drawing with {:?}", adapter.get_info());
                return Ok(Arc::new(Descriptors::new(instance, adapter, device, queue)));
            }
            Err(e) => last = format!("{b:?}: {e}"),
        }
    }
    Err(format!("no GPU for Flash ({last})"))
}

/// Pictures a late game may leave out in a row, from what its movie costs a call and its drawing a picture (ms).
/// Leaving a picture out only helps by the drawing it saves: a game whose drawing fits beside its movie skips
/// nothing, one that is a little short draws every second picture, and at most three in four are given up, when
/// the drawing is the heavy part. (A fixed three in four showed Bloons, 19 ms of movie and 5 of drawing, at 12
/// pictures a second to win back a fifth of its speed.)
fn skips_allowed(movie_ms: f64, draw_ms: f64) -> u32 {
    let spare = 1000.0 / FPS - movie_ms;
    let every = if spare > 0.0 {
        (draw_ms / spare).ceil().clamp(1.0, 4.0)
    } else if draw_ms < movie_ms / 2.0 {
        2.0
    } else {
        3.0
    };
    every as u32 - 1
}

/// A cost smoothed over about ten samples, a stall (loading) counted as no more than 50 ms.
fn smoothed(mean: f64, sample: Duration) -> f64 {
    mean + (sample.as_secs_f64().min(0.05) * 1e3 - mean) * 0.1
}

/// Frame skip is on unless RGDS_FLASH_FRAMESKIP=0.
fn frame_skip() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("RGDS_FLASH_FRAMESKIP").as_deref() != Ok("0"))
}

/// RGDS_FLASH_SKIP, a part of the picture left out to measure the rest: "render" or "present".
fn skip() -> Option<&'static str> {
    static SKIP: std::sync::OnceLock<Option<String>> = std::sync::OnceLock::new();
    SKIP.get_or_init(|| std::env::var("RGDS_FLASH_SKIP").ok()).as_deref()
}

fn quality(name: &str) -> StageQuality {
    match name {
        // High is Flash's own default, what the games were made to look like (4x anti-aliasing, smoothed bitmaps);
        // on the player's context it costs about what Medium does (HappyWheels 2.8 ms a picture on the RG DS).
        "low" => StageQuality::Low,
        "medium" => StageQuality::Medium,
        "best" => StageQuality::Best,
        _ => StageQuality::High,
    }
}

fn save_root(content_path: &Path) -> PathBuf {
    let base = env_dir(lr::ENV_GET_SAVE_DIRECTORY).unwrap_or_else(std::env::temp_dir);
    let stem = content_path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    base.join(stem)
}

/// The picture's size for a movie: its stage fitted into the panel.
fn picture_size(content: &Content) -> Result<(u32, u32), String> {
    let header = ruffle_core::swf::decompress_swf(&content.movie[..]).map_err(|e| format!("not a Flash movie: {e}"))?;
    let stage = header.header.stage_size();
    Ok(fit(stage.width().to_pixels(), stage.height().to_pixels()))
}

impl Game {
    /// `on_player`: wgpu on the player's GLES context and the blit into its framebuffer; None draws with a GPU device
    /// of the core's own and reads each picture back.
    fn load(path: &Path, on_player: Option<(Arc<Descriptors>, gl::Output)>) -> Result<Game, String> {
        let content = Arc::new(Content::open(path)?);
        let (width, height) = picture_size(&content)?;
        tracing::info!("flash: {} at {width}x{height} from {}", content.launch, path.display());

        let (descriptors, out) = match on_player {
            Some((descriptors, out)) => (descriptors, Some(out)),
            None => (gpu()?, None),
        };
        // RGDS_FLASH_NO_COPY=1 (measuring only): drawn on a device of its own, never copied back.
        let copy_back = out.is_none() && std::env::var_os("RGDS_FLASH_NO_COPY").is_none();
        // A game's own lines in its .flashpoint (`touch=hover`, `btn_a=up`, `quality=low`...) before the options.
        let option =
            |key: &str| content.details.get(key.trim_start_matches("ruffle_")).cloned().or_else(|| variable(key));
        let q = quality(&option("ruffle_quality").unwrap_or_default());
        // On the player's context anti-aliasing is FXAA (`smooth`); drawn on a device of its own (a Mac) Ruffle's MSAA
        // as it is. Low: neither. RGDS_FLASH_MSAA=1 keeps Ruffle's MSAA on the context too (measuring).
        let smooth = out.is_some() && q != StageQuality::Low && std::env::var_os("RGDS_FLASH_MSAA").is_none();
        let res = std::env::var("RGDS_FLASH_RES").ok().and_then(|s| s.parse::<u32>().ok()).unwrap_or(100).clamp(25, 100);
        let drawn = if out.is_some() { ((width * res / 100).max(1), (height * res / 100).max(1)) } else { (width, height) };
        let target = FlashTarget::new(&descriptors.device, drawn, copy_back);
        let mut renderer = WgpuRenderBackend::new(descriptors, target).map_err(|e| e.to_string())?;
        if smooth {
            // The renderer never multisamples here, whatever quality the movie sets, now or later.
            renderer.pin_quality(StageQuality::Low);
        }

        let executor = NullExecutor::new();
        let navigator = navigator::PackNavigator::new(content.clone(), executor.spawner());
        let mixer = AudioMixer::new(2, SAMPLE_RATE);
        let proxy = mixer.proxy();
        let shared = Arc::new(ui::Shared::default());
        let storage = storage::FolderStorage::new(save_root(path));
        let player = PlayerBuilder::new()
            .with_renderer(renderer)
            .with_audio(PulledAudio { mixer })
            .with_navigator(navigator)
            .with_storage(Box::new(storage))
            .with_ui(ui::PlayUi::new(shared.clone()))
            .with_video(ruffle_video_software::backend::SoftwareVideoBackend::new())
            .with_log(Log)
            .with_autoplay(true)
            .with_letterbox(Letterbox::On)
            .with_quality(q)
            .with_max_execution_duration(Duration::from_secs(10))
            .with_viewport_dimensions(drawn.0, drawn.1, drawn.0 as f64 / width as f64)
            .with_page_url(Some(content.launch.clone()))
            .build();
        {
            let mut p = player.lock().map_err(|_| "player lock")?;
            p.fetch_root_movie(content.launch.clone(), content.parameters.clone(), Box::new(|_| {}));
        }

        let binds = BUTTONS
            .map(|(_, key, default)| keys::bind_named(&button_choice(key, default, &content.details, variable(key))));
        let touch = if option("ruffle_touch").as_deref() == Some("hover") { Touch::Hover } else { Touch::Direct };
        let cursor_speed = option("ruffle_cursor_speed").and_then(|s| s.parse().ok()).unwrap_or(6.0);
        let stick = stick_of(&variable("ruffle_stick").unwrap_or_default(), &content.details);
        if let Stick::Keys(_) = stick {
            tracing::info!("flash: the stick presses keys");
        }
        Ok(Game {
            path: path.to_path_buf(),
            out,
            player,
            executor,
            mixer: proxy,
            shared,
            width,
            height,
            smooth,
            drawn,
            last_call: None,
            behind: 0.0,
            skipped: 0,
            movie_ms: 0.0,
            draw_ms: 0.0,
            clean: vec![0; (width * height) as usize],
            frame: vec![0; (width * height) as usize],
            drawn_pointer: None,
            stick,
            stick_held: [false; 4],
            binds,
            held: [false; 14],
            touch,
            finger: None,
            finger_start: None,
            finger_moved: false,
            cursor: (width as f64 / 2.0, height as f64 / 2.0),
            cursor_shown: 0,
            cursor_speed,
            left_held: 0,
            right_held: 0,
            tap_frames: 0,
            frames: 0,
            spent: Default::default(),
            audio: Vec::new(),
        })
    }

    fn send(&self, event: PlayerEvent) {
        if let Ok(mut p) = self.player.lock() {
            p.handle_event(event);
        }
    }

    /// A point on the picture in the pixels Ruffle draws.
    fn on_drawn(&self, (x, y): (f64, f64)) -> (f64, f64) {
        (x * self.drawn.0 as f64 / self.width as f64, y * self.drawn.1 as f64 / self.height as f64)
    }

    fn mouse_to(&mut self, x: f64, y: f64) {
        self.cursor = (x.clamp(0.0, self.width as f64 - 1.0), y.clamp(0.0, self.height as f64 - 1.0));
        let (x, y) = self.on_drawn(self.cursor);
        self.send(PlayerEvent::MouseMove { x, y });
    }

    /// `holder` presses or lets go of a mouse button; the game sees it go down with the first and up with the last.
    fn button(&mut self, button: MouseButton, holder: u32, down: bool) {
        let (x, y) = self.on_drawn(self.cursor);
        let held = match button {
            MouseButton::Right => &mut self.right_held,
            _ => &mut self.left_held,
        };
        let before = *held;
        *held = if down { before | holder } else { before & !holder };
        if (before != 0) == (*held != 0) {
            return;
        }
        self.send(if down {
            PlayerEvent::MouseDown { x, y, button, index: None }
        } else {
            PlayerEvent::MouseUp { x, y, button }
        });
    }

    fn poll_touch(&mut self) {
        let raw_x = input(0, lr::DEVICE_POINTER, 0, lr::POINTER_X);
        let raw_y = input(0, lr::DEVICE_POINTER, 0, lr::POINTER_Y);
        let pressed = input(0, lr::DEVICE_POINTER, 0, lr::POINTER_PRESSED) != 0;
        if self.tap_frames > 0 {
            self.tap_frames -= 1;
            if self.tap_frames == 0 {
                self.button(MouseButton::Left, HELD_TAP, false);
            }
        }
        let on_picture = raw_x != -0x8000 && raw_y != -0x8000;
        let at = on_picture.then(|| {
            (
                (raw_x as f64 + 32767.0) * self.width as f64 / 65535.0,
                (raw_y as f64 + 32767.0) * self.height as f64 / 65535.0,
            )
        });
        match (pressed, at, self.finger) {
            (true, Some((x, y)), None) => {
                self.cursor_shown = 0;
                self.mouse_to(x, y);
                self.finger = Some((x, y));
                self.finger_start = Some((x, y, self.frames));
                self.finger_moved = false;
                if self.touch == Touch::Direct {
                    self.button(MouseButton::Left, HELD_FINGER, true);
                }
            }
            (true, Some((x, y)), Some(_)) => {
                if let Some((sx, sy, _)) = self.finger_start
                    && ((x - sx).powi(2) + (y - sy).powi(2)) > 100.0
                {
                    self.finger_moved = true;
                }
                self.mouse_to(x, y);
                self.finger = Some((x, y));
            }
            (false, _, Some(_)) | (true, None, Some(_)) => {
                if self.touch == Touch::Direct {
                    self.button(MouseButton::Left, HELD_FINGER, false);
                } else if let Some((_, _, since)) = self.finger_start {
                    // Hover: a short touch that stayed put is a click where the pointer rests.
                    if !self.finger_moved && self.frames.wrapping_sub(since) < 20 {
                        self.button(MouseButton::Left, HELD_TAP, true);
                        self.tap_frames = 3;
                    }
                }
                self.finger = None;
                self.finger_start = None;
            }
            _ => {}
        }
    }

    fn poll_pad(&mut self) {
        for (i, (id, _, _)) in BUTTONS.iter().enumerate() {
            let down = input(0, lr::DEVICE_JOYPAD, 0, *id) != 0;
            if down == self.held[i] {
                continue;
            }
            self.held[i] = down;
            match self.binds[i] {
                Bind::Key(key) => self.key(key, down, None),
                Bind::MouseLeft => self.button(MouseButton::Left, HELD_PAD_BUTTON << i, down),
                Bind::MouseRight => self.button(MouseButton::Right, HELD_PAD_BUTTON << i, down),
                Bind::Nothing => {}
            }
        }
        let sx = input(0, lr::DEVICE_ANALOG, lr::ANALOG_LEFT, lr::ANALOG_X) as f64 / 32767.0;
        let sy = input(0, lr::DEVICE_ANALOG, lr::ANALOG_LEFT, lr::ANALOG_Y) as f64 / 32767.0;
        if let Stick::Keys(keys) = self.stick {
            // Four keys, each pressed past half a lean and let go under a third (no chatter at the edge); a diagonal
            // holds two, as a keyboard's arrows would.
            for (i, lean) in [-sy, sy, -sx, sx].into_iter().enumerate() {
                let down = if self.stick_held[i] { lean > 0.35 } else { lean > 0.5 };
                if down != self.stick_held[i] {
                    self.stick_held[i] = down;
                    self.key(keys[i], down, None);
                }
            }
            return;
        }
        // The left stick moves the pointer, faster the further it leans.
        let lean = (sx * sx + sy * sy).sqrt();
        if lean > 0.18 && self.finger.is_none() {
            let gain = self.cursor_speed * ((lean - 0.18) / 0.82).powf(1.6) / lean.max(1e-3);
            let (x, y) = self.cursor;
            self.mouse_to(x + sx * gain * self.width as f64 / 160.0, y + sy * gain * self.width as f64 / 160.0);
            self.cursor_shown = 180;
        }
    }

    /// The second screen's touch pad (the keyboard's Mouse page): motion and buttons, as a laptop's pad, for a game
    /// on the top screen that aims with the pointer.
    fn poll_pad_mouse(&mut self) {
        let dx = input(0, lr::DEVICE_MOUSE, 0, lr::MOUSE_X) as f64;
        let dy = input(0, lr::DEVICE_MOUSE, 0, lr::MOUSE_Y) as f64;
        let left = input(0, lr::DEVICE_MOUSE, 0, lr::MOUSE_LEFT) != 0;
        let right = input(0, lr::DEVICE_MOUSE, 0, lr::MOUSE_RIGHT) != 0;
        if dx != 0.0 || dy != 0.0 {
            let (x, y) = self.cursor;
            self.mouse_to(x + dx, y + dy);
            self.cursor_shown = 180;
        }
        if left != (self.left_held & HELD_TOUCH_PAD != 0) {
            self.button(MouseButton::Left, HELD_TOUCH_PAD, left);
            self.cursor_shown = self.cursor_shown.max(60);
        }
        if right != (self.right_held & HELD_TOUCH_PAD != 0) {
            self.button(MouseButton::Right, HELD_TOUCH_PAD, right);
        }
    }

    fn poll_typed(&mut self) {
        let typed: Vec<_> = TYPED.lock().map(|mut q| std::mem::take(&mut *q)).unwrap_or_default();
        for (down, code, ch) in typed {
            let Some(key) = keys::retro_key(code, ch) else { continue };
            self.key(key, down, char::from_u32(ch).filter(|c| !c.is_control()));
        }
    }

    /// A key down or up, and on the way down what it types, as a desktop's keyboard sends them: a character as text,
    /// Enter and Backspace as editing, Tab as a tab.
    fn key(&mut self, key: KeyDescriptor, down: bool, typed: Option<char>) {
        use ruffle_core::events::{LogicalKey, NamedKey, TextControlCode};
        if !down {
            self.send(PlayerEvent::KeyUp { key });
            return;
        }
        self.send(PlayerEvent::KeyDown { key });
        match (typed, key.logical_key) {
            (Some(c), _) | (None, LogicalKey::Character(c)) => self.send(PlayerEvent::TextInput { codepoint: c }),
            (None, LogicalKey::Named(NamedKey::Enter)) => {
                self.send(PlayerEvent::TextControl { code: TextControlCode::Enter })
            }
            (None, LogicalKey::Named(NamedKey::Backspace)) => {
                self.send(PlayerEvent::TextControl { code: TextControlCode::Backspace })
            }
            (None, LogicalKey::Named(NamedKey::Delete)) => {
                self.send(PlayerEvent::TextControl { code: TextControlCode::Delete })
            }
            (None, LogicalKey::Named(NamedKey::ArrowLeft)) => {
                self.send(PlayerEvent::TextControl { code: TextControlCode::MoveLeft })
            }
            (None, LogicalKey::Named(NamedKey::ArrowRight)) => {
                self.send(PlayerEvent::TextControl { code: TextControlCode::MoveRight })
            }
            (None, LogicalKey::Named(NamedKey::Tab)) => self.send(PlayerEvent::TextInput { codepoint: '\t' }),
            _ => {}
        }
    }

    fn run(&mut self) {
        self.frames = self.frames.wrapping_add(1);
        let now = Instant::now();
        if let Some(last) = self.last_call {
            let gap = (now - last).as_secs_f64();
            // A gap of more than a quarter second is a pause (the menu), not lateness.
            self.behind = if gap > 0.25 { 0.0 } else { (self.behind + gap - 1.0 / FPS).clamp(0.0, 0.25) };
        }
        self.last_call = Some(now);
        self.poll_typed();
        self.poll_touch();
        self.poll_pad();
        self.poll_pad_mouse();

        let mut drew = false;
        if let Ok(mut p) = self.player.lock() {
            let at = Instant::now();
            p.tick(FloatDuration::from_secs(1.0 / FPS));
            self.spent.add(0, at);
            self.movie_ms = smoothed(self.movie_ms, at.elapsed());
            drop(p);
            let at = Instant::now();
            self.executor.run();
            self.spent.add(1, at);
            let mut p = self.player.lock().unwrap();
            // Behind by more than a frame: this picture is left out (the next one is drawn), as many in a row as
            // skips_allowed says. needs_render stays set, so the skipped change shows in the next picture.
            let late = frame_skip()
                && self.behind > 1.0 / FPS
                && self.skipped < skips_allowed(self.movie_ms, self.draw_ms);
            if p.needs_render() && late && self.frames >= 3 {
                self.skipped += 1;
                self.spent.skipped += 1;
            } else if p.needs_render() || self.frames < 3 {
                self.skipped = 0;
                let at = Instant::now();
                let cpu = thread_cpu();
                // RGDS_FLASH_SKIP=render (measuring): the movie runs, Ruffle draws nothing, the last picture is shown.
                if skip() != Some("render") {
                    p.render();
                }
                self.spent.add(2, at);
                self.spent.draw_cpu += thread_cpu().saturating_sub(cpu);
                self.draw_ms = smoothed(self.draw_ms, at.elapsed());
                drew = true;
            }
        }
        let at = Instant::now();
        if drew {
            if self.out.is_none() {
                self.capture();
            }
            self.spent.pictures += 1;
        }
        if self.frames.is_multiple_of(600) {
            let s = std::mem::take(&mut self.spent);
            let ms = |d: Duration| d.as_secs_f64() * 1e3;
            let part = |i: usize| format!("{:.2} (worst {:.1})", ms(s.parts[i].0) / 600.0, ms(s.parts[i].1));
            tracing::debug!(
                "flash: {} pictures ({} skipped) in 600 frames; ms a frame: movie {}, loaders {}, drawing {} (CPU {:.2}), {} {} (copy {:.2}), sound {}",
                s.pictures,
                s.skipped,
                part(0),
                part(1),
                part(2),
                ms(s.draw_cpu) / 600.0,
                if self.out.is_some() { "blit" } else { "read-back" },
                part(3),
                ms(s.copy) / 600.0,
                part(4)
            );
        }
        // The stick's pointer is drawn over the last picture: moving it costs a copy (or a blit), not a render.
        let pointer = (self.cursor_shown > 0 && self.shared.mouse_visible.load(Ordering::Relaxed))
            .then_some((self.cursor.0 as i64, self.cursor.1 as i64));
        let present = drew || pointer != self.drawn_pointer;
        let mut on_player = false;
        if present {
            if self.out.is_some()
                && let Some(texture) = self.texture()
            {
                let (w, h) = (self.width, self.height);
                // RGDS_FLASH_SKIP=present (measuring): drawn but not put on the player's framebuffer.
                on_player = skip() == Some("present")
                    || self.out.as_mut().is_some_and(|out| out.present(&texture, w, h, self.smooth, self.drawn, pointer));
            }
            if !on_player {
                self.frame.copy_from_slice(&self.clean);
                if let Some(at) = pointer {
                    draw_pointer(&mut self.frame, self.width, self.height, at);
                }
            }
            self.drawn_pointer = pointer;
        }
        self.spent.add(3, at);

        let at = Instant::now();
        // 44.1 kHz at 60 frames a second is 735 sample pairs a frame.
        let pairs = (SAMPLE_RATE as f64 / FPS).round() as usize;
        self.audio.resize(pairs * 2, 0);
        self.mixer.mix::<i16>(&mut self.audio);
        let f = AUDIO_BATCH.load(Ordering::Acquire);
        if f != 0 {
            let f: lr::AudioSampleBatchFn = unsafe { std::mem::transmute(f) };
            unsafe { f(self.audio.as_ptr(), pairs) };
        }
        self.spent.add(4, at);

        let f = VIDEO.load(Ordering::Acquire);
        if f != 0 {
            let f: lr::VideoRefreshFn = unsafe { std::mem::transmute(f) };
            if on_player {
                unsafe { f(lr::HW_FRAME_BUFFER_VALID, self.width, self.height, 0) };
            } else if present {
                unsafe { f(self.frame.as_ptr() as *const c_void, self.width, self.height, self.width as usize * 4) };
            } else {
                unsafe { f(std::ptr::null(), self.width, self.height, self.width as usize * 4) };
            }
        }
        self.cursor_shown = self.cursor_shown.saturating_sub(1);
    }

    /// The texture Ruffle draws into.
    fn texture(&self) -> Option<wgpu::Texture> {
        let mut p = self.player.lock().ok()?;
        let r = <dyn std::any::Any>::downcast_mut::<WgpuRenderBackend<FlashTarget>>(p.renderer_mut())?;
        Some(r.target().texture.clone())
    }

    /// Reads the picture just drawn into `clean`, in one pass straight from the mapped buffer. The movie is drawn
    /// opaque (letterbox and stage both), so its premultiplied RGBA is already the colour; Ruffle's own capture would
    /// copy it twice and divide every pixel by its alpha.
    fn capture(&mut self) {
        let player = self.player.clone();
        let mut p = player.lock().unwrap();
        let Some(r) = <dyn std::any::Any>::downcast_mut::<WgpuRenderBackend<FlashTarget>>(p.renderer_mut()) else {
            return;
        };
        let Some((buffer, dims)) = &r.target().buffer else { return };
        let (w, h) = (dims.width.min(self.width as usize), dims.height.min(self.height as usize));
        let stride = self.width as usize;
        let clean = &mut self.clean;
        let mut copy = Duration::ZERO;
        ruffle_render_wgpu::utils::capture_image(r.device(), buffer, dims, None, |rgba, padded| {
            let at = Instant::now();
            for (y, row) in rgba.chunks(padded as usize).take(h).enumerate() {
                let out = &mut clean[y * stride..y * stride + w];
                for (d, px) in out.iter_mut().zip(row[..w * 4].as_chunks::<4>().0) {
                    *d = (px[0] as u32) << 16 | (px[1] as u32) << 8 | px[2] as u32;
                }
            }
            copy = at.elapsed();
        });
        self.spent.copy += copy;
    }
}

/// Time spent in each part of a frame, and the worst frame of each, since the last report.
#[derive(Default)]
struct Spent {
    /// running the movie, its loaders, drawing, reading back or blitting, mixing the sound
    parts: [(Duration, Duration); 5],
    pictures: u32,
    /// pictures frame skip left out
    skipped: u32,
    /// CPU time of the drawing part (the rest of its wall time is waiting)
    draw_cpu: Duration,
    /// of reading back: copying the mapped picture (the rest is waiting for the GPU)
    copy: Duration,
}

/// CPU time this thread has used.
fn thread_cpu() -> Duration {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut t) };
    Duration::new(t.tv_sec as u64, t.tv_nsec as u32)
}

impl Spent {
    fn add(&mut self, part: usize, since: Instant) {
        let d = since.elapsed();
        let p = &mut self.parts[part];
        p.0 += d;
        p.1 = p.1.max(d);
    }
}

/// The stick's pointer, row by row: X the rim, o the inside.
pub(crate) const ARROW: [&str; 12] = [
    "X.........",
    "XX........",
    "XoX.......",
    "XooX......",
    "XoooX.....",
    "XooooX....",
    "XoooooX...",
    "XooooooX..",
    "XoooXXXXX.",
    "XooX......",
    "XoX.......",
    "XX........",
];

/// A small arrow at the stick's pointer (a finger needs none), white with a dark rim.
fn draw_pointer(frame: &mut [u32], w: u32, h: u32, at: (i64, i64)) {
    let scale = 2usize;
    let (cx, cy) = at;
    for (row, line) in ARROW.iter().enumerate() {
        for (col, c) in line.bytes().enumerate() {
            let colour = match c {
                b'X' => 0x202020,
                b'o' => 0xFFFFFF,
                _ => continue,
            };
            for dy in 0..scale {
                for dx in 0..scale {
                    let x = cx + (col * scale + dx) as i64;
                    let y = cy + (row * scale + dy) as i64;
                    if x >= 0 && y >= 0 && (x as u32) < w && (y as u32) < h {
                        frame[y as usize * w as usize + x as usize] = colour;
                    }
                }
            }
        }
    }
}

struct Log;
impl ruffle_core::backend::log::LogBackend for Log {
    fn avm_trace(&self, message: &str) {
        tracing::debug!("trace: {message}");
    }
    fn avm_warning(&self, message: &str) {
        tracing::warn!("avm: {message}");
    }
}

// ---- libretro entry points ----

#[unsafe(no_mangle)]
pub extern "C" fn retro_api_version() -> c_uint {
    lr::API_VERSION
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_set_environment(f: lr::EnvironmentFn) {
    ENV.store(f as usize, Ordering::Release);
    struct Vars(#[allow(dead_code)] Vec<CString>, Vec<lr::Variable>);
    // The strings never change once made; the pointers into them are only read.
    unsafe impl Send for Vars {}
    unsafe impl Sync for Vars {}
    static VARS: std::sync::OnceLock<Vars> = std::sync::OnceLock::new();
    let Vars(_, vars) = VARS.get_or_init(|| {
        let keys = "space|z|x|c|v|enter|shift|ctrl|alt|escape|tab|backspace|up|down|left|right|w|a|s|d|q|e|r|f|p|m|1|2|3|mouse|right click|none";
        let mut owned = Vec::new();
        let mut add = |k: &str, v: String| owned.push((CString::new(k).unwrap(), CString::new(v).unwrap()));
        add("ruffle_quality", "Picture quality; high|low|medium|best".into());
        add("ruffle_touch", "Touch; direct|hover".into());
        add("ruffle_stick", "Left stick; auto|pointer|arrows|wasd".into());
        add("ruffle_cursor_speed", "Stick pointer speed; 6|3|4|5|7|8|10|12".into());
        for (_, key, default) in BUTTONS {
            // The default first, then every other choice.
            let rest: Vec<_> = keys.split('|').filter(|k| *k != default).collect();
            add(key, format!("{key}; {default}|{}", rest.join("|")));
        }
        let mut vars: Vec<lr::Variable> =
            owned.iter().map(|(k, v)| lr::Variable { key: k.as_ptr(), value: v.as_ptr() }).collect();
        vars.push(lr::Variable { key: std::ptr::null(), value: std::ptr::null() });
        let names: Vec<CString> = owned.into_iter().flat_map(|(k, v)| [k, v]).collect();
        Vars(names, vars)
    });
    env(lr::ENV_SET_VARIABLES, vars.as_ptr() as *mut c_void);
    let cb = lr::KeyboardCallback { callback: keyboard_event };
    env(lr::ENV_SET_KEYBOARD_CALLBACK, &cb as *const _ as *mut c_void);
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_set_video_refresh(f: lr::VideoRefreshFn) {
    VIDEO.store(f as usize, Ordering::Release);
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_set_audio_sample(_f: lr::AudioSampleFn) {}
#[unsafe(no_mangle)]
pub extern "C" fn retro_set_audio_sample_batch(f: lr::AudioSampleBatchFn) {
    AUDIO_BATCH.store(f as usize, Ordering::Release);
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_set_input_poll(f: lr::InputPollFn) {
    POLL.store(f as usize, Ordering::Release);
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_set_input_state(f: lr::InputStateFn) {
    INPUT.store(f as usize, Ordering::Release);
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_init() {
    // Ruffle's warnings stay out of the log unless asked for: a movie with a tag it does not know says so for every
    // clip on every frame (Swords and Sandals: 800 lines a second, each formatted and written to the player's pipe).
    let filter = std::env::var("RGDS_FLASH_LOG").unwrap_or_else(|_| "error,ruffle_libretro=info".into());
    let _ = tracing_subscriber::fmt()
        .with_env_filter(tracing_subscriber::EnvFilter::new(filter))
        .with_writer(std::io::stderr)
        .with_target(false)
        .try_init();
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_deinit() {}

#[unsafe(no_mangle)]
pub extern "C" fn retro_get_system_info(info: *mut lr::SystemInfo) {
    static NAME: &CStr = c"Ruffle";
    static VERSION: &CStr = c"0.8.0";
    static EXTS: &CStr = c"swf|spl|zip";
    if let Some(info) = unsafe { info.as_mut() } {
        info.library_name = NAME.as_ptr();
        info.library_version = VERSION.as_ptr();
        info.valid_extensions = EXTS.as_ptr();
        info.need_fullpath = true;
        info.block_extract = true;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_get_system_av_info(info: *mut lr::SystemAvInfo) {
    // Before the context comes (a game drawn on the player's), the size its movie will have.
    let pending = PENDING.lock().ok().and_then(|p| p.as_ref().map(|p| p.size));
    let (w, h) =
        GAME.lock().ok().and_then(|g| g.0.as_ref().map(|g| (g.width, g.height))).or(pending).unwrap_or((MAX_W, MAX_H));
    if let Some(info) = unsafe { info.as_mut() } {
        info.geometry = lr::GameGeometry {
            base_width: w,
            base_height: h,
            max_width: MAX_W,
            max_height: MAX_H,
            aspect_ratio: w as f32 / h as f32,
        };
        info.timing = lr::SystemTiming { fps: FPS, sample_rate: SAMPLE_RATE as f64 };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_set_controller_port_device(_port: c_uint, _device: c_uint) {}

#[unsafe(no_mangle)]
pub extern "C" fn retro_reset() {
    // Flash has no reset line: the movie is loaded again from the start, its saves written first.
    let Ok(mut slot) = GAME.lock() else { return };
    let Some(mut game) = slot.0.take() else { return };
    if let Ok(mut p) = game.player.lock() {
        p.flush_shared_objects();
    }
    let path = game.path.clone();
    // On the player's context: the same device and framebuffer again.
    let on_player = game.out.take().and_then(|out| {
        let mut p = game.player.lock().ok()?;
        let r = <dyn std::any::Any>::downcast_mut::<WgpuRenderBackend<FlashTarget>>(p.renderer_mut())?;
        Some((r.descriptors().clone(), out))
    });
    drop(game);
    match Game::load(&path, on_player) {
        Ok(g) => slot.0 = Some(g),
        Err(e) => tracing::error!("flash: reset: {e}"),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_run() {
    let f = POLL.load(Ordering::Acquire);
    if f != 0 {
        let f: lr::InputPollFn = unsafe { std::mem::transmute(f) };
        unsafe { f() };
    }
    if let Ok(mut g) = GAME.lock()
        && let Some(game) = g.0.as_mut()
    {
        game.run();
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_serialize_size() -> usize {
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_serialize(_data: *mut c_void, _size: usize) -> bool {
    false
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_unserialize(_data: *const c_void, _size: usize) -> bool {
    false
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_cheat_reset() {}
#[unsafe(no_mangle)]
pub extern "C" fn retro_cheat_set(_index: c_uint, _enabled: bool, _code: *const c_char) {}

#[unsafe(no_mangle)]
pub extern "C" fn retro_load_game(game: *const lr::GameInfo) -> bool {
    let Some(game) = (unsafe { game.as_ref() }) else { return false };
    if game.path.is_null() {
        return false;
    }
    let path = PathBuf::from(unsafe { CStr::from_ptr(game.path) }.to_string_lossy().into_owned());
    let mut format = lr::PIXEL_FORMAT_XRGB8888;
    if !env(lr::ENV_SET_PIXEL_FORMAT, &mut format as *mut _ as *mut c_void) {
        tracing::error!("flash: the player refused XRGB8888");
        return false;
    }
    // On the handheld: Ruffle draws on the player's GLES context, the game made when the context comes
    // (context_reset); RGDS_FLASH_SOFTWARE=1 keeps the read-back.
    if cfg!(target_os = "linux") && std::env::var_os("RGDS_FLASH_SOFTWARE").is_none() {
        let size = Content::open(&path).and_then(|c| picture_size(&c));
        let mut hw = lr::HwRenderCallback {
            context_type: lr::HW_CONTEXT_OPENGLES3,
            context_reset: Some(context_reset),
            get_current_framebuffer: None,
            get_proc_address: None,
            depth: false,
            stencil: false,
            bottom_left_origin: false,
            version_major: 3,
            version_minor: 0,
            cache_context: false,
            context_destroy: Some(context_destroy),
            debug_context: false,
        };
        if let Ok(size) = size
            && env(lr::ENV_SET_HW_RENDER, &mut hw as *mut _ as *mut c_void)
            && let (Some(framebuffer), Some(proc_address)) = (hw.get_current_framebuffer, hw.get_proc_address)
        {
            if let Ok(mut p) = PENDING.lock() {
                *p = Some(Pending { path, size, host: gl_host(framebuffer, proc_address) });
            }
            return true;
        }
    }
    match std::panic::catch_unwind(|| Game::load(&path, None)) {
        Ok(Ok(g)) => {
            if let Ok(mut slot) = GAME.lock() {
                slot.0 = Some(g);
            }
            true
        }
        Ok(Err(e)) => {
            tracing::error!("flash: {e}");
            false
        }
        Err(_) => {
            tracing::error!("flash: the player crashed loading {}", path.display());
            false
        }
    }
}

/// A game waiting for the player's context.
struct Pending {
    path: PathBuf,
    size: (u32, u32),
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
    host: Option<gl::Host>,
}
static PENDING: Mutex<Option<Pending>> = Mutex::new(None);

#[cfg(target_os = "linux")]
fn gl_host(framebuffer: lr::HwGetCurrentFramebufferFn, proc_address: lr::HwGetProcAddressFn) -> Option<gl::Host> {
    Some(gl::Host { framebuffer, proc_address })
}
#[cfg(not(target_os = "linux"))]
fn gl_host(_f: lr::HwGetCurrentFramebufferFn, _p: lr::HwGetProcAddressFn) -> Option<gl::Host> {
    None
}

/// The player's context is current: the game is made on it (or, when wgpu can't use it, with a device of its own,
/// its pictures read back and handed over as before).
unsafe extern "C" fn context_reset() {
    let Some(pending) = PENDING.lock().ok().and_then(|mut p| p.take()) else { return };
    let path = pending.path.clone();
    let made = std::panic::catch_unwind(|| {
        #[cfg(target_os = "linux")]
        if let Some(host) = pending.host {
            match gl::adopt(host).and_then(|d| Game::load(&path, Some((d, gl::Output::new(host))))) {
                Ok(g) => return Ok(g),
                Err(e) => tracing::warn!("flash: {e}; reading the pictures back instead"),
            }
        }
        let _ = &pending;
        Game::load(&path, None)
    });
    match made {
        Ok(Ok(g)) => {
            if let Ok(mut slot) = GAME.lock() {
                slot.0 = Some(g);
            }
        }
        Ok(Err(e)) => tracing::error!("flash: {e}"),
        Err(_) => tracing::error!("flash: the player crashed loading {}", pending.path.display()),
    }
}

/// The context goes away: the saves written, the game (and its GL objects, the context still current) let go.
unsafe extern "C" fn context_destroy() {
    if let Ok(mut slot) = GAME.lock()
        && let Some(game) = slot.0.take()
    {
        if let Ok(mut p) = game.player.lock() {
            p.flush_shared_objects();
        }
        drop(game);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_load_game_special(_type: c_uint, _info: *const lr::GameInfo, _num: usize) -> bool {
    false
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_unload_game() {
    if let Ok(mut p) = PENDING.lock() {
        *p = None;
    }
    if let Ok(mut slot) = GAME.lock()
        && let Some(game) = slot.0.take()
    {
        if let Ok(mut p) = game.player.lock() {
            p.flush_shared_objects();
        }
        drop(game);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_get_region() -> c_uint {
    0
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_get_memory_data(_id: c_uint) -> *mut c_void {
    std::ptr::null_mut()
}
#[unsafe(no_mangle)]
pub extern "C" fn retro_get_memory_size(_id: c_uint) -> usize {
    0
}

/// RG DS hook: 1 while a text field in the game has focus and wants typing (the player may offer the on-screen
/// keyboard).
#[unsafe(no_mangle)]
pub extern "C" fn retro_rgds_wants_keyboard() -> i32 {
    GAME.lock()
        .ok()
        .and_then(|g| g.0.as_ref().map(|g| g.shared.wants_keyboard.load(Ordering::Relaxed) as i32))
        .unwrap_or(0)
}

/// RG DS hook: saves Flash's local saves (SharedObjects) now, before the player is stopped.
#[unsafe(no_mangle)]
pub extern "C" fn retro_rgds_flush_saves() {
    if let Ok(mut g) = GAME.lock()
        && let Some(game) = g.0.as_mut()
        && let Ok(mut p) = game.player.lock()
    {
        p.flush_shared_objects();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stick_by_tags_and_line() {
        let mut d = std::collections::HashMap::new();
        // Frame skip: nothing while the picture fits beside the movie, every second when the movie alone is late,
        // more only when the drawing is the heavy part.
        assert_eq!(skips_allowed(5.0, 5.0), 0);
        assert_eq!(skips_allowed(12.0, 8.0), 1);
        assert_eq!(skips_allowed(15.0, 20.0), 3);
        assert_eq!(skips_allowed(19.4, 5.4), 1);
        assert_eq!(skips_allowed(20.0, 30.0), 2);
        assert!(matches!(stick_of("auto", &d), Stick::Pointer));
        d.insert("tags".into(), "Arcade, Racing, Cars".into());
        assert!(matches!(stick_of("auto", &d), Stick::Keys(_)));
        assert!(matches!(stick_of("pointer", &d), Stick::Pointer));
        assert_eq!(button_choice("ruffle_btn_a", "space", &d, Some("space".into())), "up");
        assert_eq!(button_choice("ruffle_btn_a", "space", &d, Some("x".into())), "x");
        assert_eq!(button_choice("ruffle_btn_y", "c", &d, Some("c".into())), "c");
        d.insert("btn_a".into(), "q".into());
        assert_eq!(button_choice("ruffle_btn_a", "space", &d, Some("x".into())), "q");
        d.insert("stick".into(), "pointer".into());
        assert!(matches!(stick_of("arrows", &d), Stick::Pointer));
    }
}
