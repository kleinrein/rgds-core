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

mod content;
mod keys;
pub mod libretro;
mod navigator;
mod storage;
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
use ruffle_render_wgpu::target::TextureTarget;
use ruffle_render_wgpu::wgpu;

use crate::content::Content;
use crate::keys::Bind;
use crate::libretro as lr;

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
    (lr::JOY_L, "ruffle_btn_l", "shift"),
    (lr::JOY_R, "ruffle_btn_r", "mouse"),
    (lr::JOY_L2, "ruffle_btn_l2", "ctrl"),
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
    left_down: bool,
    right_down: bool,
    pad_left: bool,
    pad_right: bool,
    frames: u32,
    /// Time spent drawing and reading back since the last report (RGDS_FLASH_LOG=debug), and how many draws.
    spent: (Duration, Duration, u32),
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

fn quality(name: &str) -> StageQuality {
    match name {
        "low" => StageQuality::Low,
        "high" => StageQuality::High,
        "best" => StageQuality::Best,
        _ => StageQuality::Medium,
    }
}

fn save_root(content_path: &Path) -> PathBuf {
    let base = env_dir(lr::ENV_GET_SAVE_DIRECTORY).unwrap_or_else(std::env::temp_dir);
    let stem = content_path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    base.join(stem)
}

impl Game {
    fn load(path: &Path) -> Result<Game, String> {
        let content = Arc::new(Content::open(path)?);
        let header =
            ruffle_core::swf::decompress_swf(&content.movie[..]).map_err(|e| format!("not a Flash movie: {e}"))?;
        let stage = header.header.stage_size();
        let (width, height) = fit(stage.width().to_pixels(), stage.height().to_pixels());
        tracing::info!("flash: {} at {width}x{height} from {}", content.launch, path.display());

        let descriptors = gpu()?;
        let target = TextureTarget::new(&descriptors.device, (width, height)).map_err(|e| e.to_string())?;
        let renderer = WgpuRenderBackend::new(descriptors, target).map_err(|e| e.to_string())?;

        let executor = NullExecutor::new();
        let navigator = navigator::PackNavigator::new(content.clone(), executor.spawner());
        let mixer = AudioMixer::new(2, SAMPLE_RATE);
        let proxy = mixer.proxy();
        let shared = Arc::new(ui::Shared::default());
        let storage = storage::FolderStorage::new(save_root(path));
        // A game's own lines in its .flashpoint (`touch=hover`, `btn_a=up`, `quality=low`...) before the options.
        let option =
            |key: &str| content.details.get(key.trim_start_matches("ruffle_")).cloned().or_else(|| variable(key));
        let q = quality(&option("ruffle_quality").unwrap_or_default());

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
            .with_viewport_dimensions(width, height, 1.0)
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
            player,
            executor,
            mixer: proxy,
            shared,
            width,
            height,
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
            left_down: false,
            right_down: false,
            pad_left: false,
            pad_right: false,
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

    fn mouse_to(&mut self, x: f64, y: f64) {
        self.cursor = (x.clamp(0.0, self.width as f64 - 1.0), y.clamp(0.0, self.height as f64 - 1.0));
        let (x, y) = self.cursor;
        self.send(PlayerEvent::MouseMove { x, y });
    }

    fn button(&mut self, button: MouseButton, down: bool) {
        let (x, y) = self.cursor;
        let held = match button {
            MouseButton::Right => &mut self.right_down,
            _ => &mut self.left_down,
        };
        if *held == down {
            return;
        }
        *held = down;
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
                    self.button(MouseButton::Left, true);
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
                    self.button(MouseButton::Left, false);
                } else if let Some((_, _, since)) = self.finger_start {
                    // Hover: a short touch that stayed put is a click where the pointer rests.
                    if !self.finger_moved && self.frames.wrapping_sub(since) < 20 {
                        self.button(MouseButton::Left, true);
                        self.button(MouseButton::Left, false);
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
                Bind::MouseLeft => self.button(MouseButton::Left, down),
                Bind::MouseRight => self.button(MouseButton::Right, down),
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
        if left != self.pad_left {
            self.pad_left = left;
            self.button(MouseButton::Left, left);
            self.cursor_shown = self.cursor_shown.max(60);
        }
        if right != self.pad_right {
            self.pad_right = right;
            self.button(MouseButton::Right, right);
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
        self.poll_typed();
        self.poll_touch();
        self.poll_pad();
        self.poll_pad_mouse();

        let mut drew = false;
        if let Ok(mut p) = self.player.lock() {
            p.tick(FloatDuration::from_secs(1.0 / FPS));
            drop(p);
            self.executor.run();
            let mut p = self.player.lock().unwrap();
            if p.needs_render() || self.frames < 3 {
                let at = Instant::now();
                p.render();
                self.spent.0 += at.elapsed();
                drew = true;
            }
        }
        if drew {
            let at = Instant::now();
            self.capture();
            self.spent.1 += at.elapsed();
            self.spent.2 += 1;
        }
        if self.frames.is_multiple_of(600) && self.spent.2 > 0 {
            let (draw, read, n) = std::mem::take(&mut self.spent);
            tracing::debug!(
                "flash: {n} pictures in 600 frames, {:.2} ms drawing and {:.2} ms reading back each",
                draw.as_secs_f64() * 1e3 / n as f64,
                read.as_secs_f64() * 1e3 / n as f64
            );
        }
        // The stick's pointer is drawn over the last picture: moving it costs a copy, not a render.
        let pointer = (self.cursor_shown > 0 && self.shared.mouse_visible.load(Ordering::Relaxed))
            .then_some((self.cursor.0 as i64, self.cursor.1 as i64));
        let present = drew || pointer != self.drawn_pointer;
        if present {
            self.frame.copy_from_slice(&self.clean);
            if let Some(at) = pointer {
                draw_pointer(&mut self.frame, self.width, self.height, at);
            }
            self.drawn_pointer = pointer;
        }

        // 44.1 kHz at 60 frames a second is 735 sample pairs a frame.
        let pairs = (SAMPLE_RATE as f64 / FPS).round() as usize;
        self.audio.resize(pairs * 2, 0);
        self.mixer.mix::<i16>(&mut self.audio);
        let f = AUDIO_BATCH.load(Ordering::Acquire);
        if f != 0 {
            let f: lr::AudioSampleBatchFn = unsafe { std::mem::transmute(f) };
            unsafe { f(self.audio.as_ptr(), pairs) };
        }

        let f = VIDEO.load(Ordering::Acquire);
        if f != 0 {
            let f: lr::VideoRefreshFn = unsafe { std::mem::transmute(f) };
            if present {
                unsafe { f(self.frame.as_ptr() as *const c_void, self.width, self.height, self.width as usize * 4) };
            } else {
                unsafe { f(std::ptr::null(), self.width, self.height, self.width as usize * 4) };
            }
        }
        self.cursor_shown = self.cursor_shown.saturating_sub(1);
    }

    /// Reads the picture just drawn into `clean`, in one pass straight from the mapped buffer. The movie is drawn
    /// opaque (letterbox and stage both), so its premultiplied RGBA is already the colour; Ruffle's own capture would
    /// copy it twice and divide every pixel by its alpha.
    fn capture(&mut self) {
        let player = self.player.clone();
        let mut p = player.lock().unwrap();
        let Some(r) = <dyn std::any::Any>::downcast_mut::<WgpuRenderBackend<TextureTarget>>(p.renderer_mut()) else {
            return;
        };
        let Some(info) = &r.target().buffer else { return };
        let (buffer, dims) = info.buffer.inner();
        let (w, h) = (dims.width.min(self.width as usize), dims.height.min(self.height as usize));
        let stride = self.width as usize;
        let clean = &mut self.clean;
        ruffle_render_wgpu::utils::capture_image(r.device(), buffer, dims, None, |rgba, padded| {
            for (y, row) in rgba.chunks(padded as usize).take(h).enumerate() {
                let out = &mut clean[y * stride..y * stride + w];
                for (d, px) in out.iter_mut().zip(row[..w * 4].as_chunks::<4>().0) {
                    *d = (px[0] as u32) << 16 | (px[1] as u32) << 8 | px[2] as u32;
                }
            }
        });
    }
}

/// A small arrow at the stick's pointer (a finger needs none), white with a dark rim.
fn draw_pointer(frame: &mut [u32], w: u32, h: u32, at: (i64, i64)) {
    const ARROW: [&str; 12] = [
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
        add("ruffle_quality", "Picture quality; medium|low|high|best".into());
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
    let filter = std::env::var("RGDS_FLASH_LOG").unwrap_or_else(|_| "warn,ruffle_libretro=info".into());
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
    let (w, h) = GAME.lock().ok().and_then(|g| g.0.as_ref().map(|g| (g.width, g.height))).unwrap_or((MAX_W, MAX_H));
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
    let Some(game) = slot.0.take() else { return };
    if let Ok(mut p) = game.player.lock() {
        p.flush_shared_objects();
    }
    let path = game.path.clone();
    drop(game);
    match Game::load(&path) {
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
    match std::panic::catch_unwind(|| Game::load(&path)) {
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

#[unsafe(no_mangle)]
pub extern "C" fn retro_load_game_special(_type: c_uint, _info: *const lr::GameInfo, _num: usize) -> bool {
    false
}

#[unsafe(no_mangle)]
pub extern "C" fn retro_unload_game() {
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
