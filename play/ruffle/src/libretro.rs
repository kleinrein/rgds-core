//! The few libretro.h types and numbers this core needs (play/src/core/libretro/libretro.h is the reference).
#![allow(dead_code)]

use std::os::raw::{c_char, c_uint, c_void};

pub const API_VERSION: c_uint = 1;

pub const DEVICE_JOYPAD: c_uint = 1;
pub const DEVICE_MOUSE: c_uint = 2;
pub const DEVICE_KEYBOARD: c_uint = 3;
pub const DEVICE_ANALOG: c_uint = 5;
pub const DEVICE_POINTER: c_uint = 6;

pub const JOY_B: c_uint = 0;
pub const JOY_Y: c_uint = 1;
pub const JOY_SELECT: c_uint = 2;
pub const JOY_START: c_uint = 3;
pub const JOY_UP: c_uint = 4;
pub const JOY_DOWN: c_uint = 5;
pub const JOY_LEFT: c_uint = 6;
pub const JOY_RIGHT: c_uint = 7;
pub const JOY_A: c_uint = 8;
pub const JOY_X: c_uint = 9;
pub const JOY_L: c_uint = 10;
pub const JOY_R: c_uint = 11;
pub const JOY_L2: c_uint = 12;
pub const JOY_R2: c_uint = 13;
pub const JOY_L3: c_uint = 14;
pub const JOY_R3: c_uint = 15;

pub const ANALOG_LEFT: c_uint = 0;
pub const ANALOG_RIGHT: c_uint = 1;
pub const ANALOG_X: c_uint = 0;
pub const ANALOG_Y: c_uint = 1;

pub const MOUSE_X: c_uint = 0;
pub const MOUSE_Y: c_uint = 1;
pub const MOUSE_LEFT: c_uint = 2;
pub const MOUSE_RIGHT: c_uint = 3;

pub const POINTER_X: c_uint = 0;
pub const POINTER_Y: c_uint = 1;
pub const POINTER_PRESSED: c_uint = 2;

pub const ENV_GET_CAN_DUPE: c_uint = 3;
pub const ENV_GET_SYSTEM_DIRECTORY: c_uint = 9;
pub const ENV_SET_PIXEL_FORMAT: c_uint = 10;
pub const ENV_SET_INPUT_DESCRIPTORS: c_uint = 11;
pub const ENV_SET_KEYBOARD_CALLBACK: c_uint = 12;
pub const ENV_GET_VARIABLE: c_uint = 15;
pub const ENV_SET_VARIABLES: c_uint = 16;
pub const ENV_GET_VARIABLE_UPDATE: c_uint = 17;
pub const ENV_GET_LOG_INTERFACE: c_uint = 27;
pub const ENV_GET_SAVE_DIRECTORY: c_uint = 31;
pub const ENV_SET_GEOMETRY: c_uint = 37;

pub const PIXEL_FORMAT_XRGB8888: c_uint = 1;

pub type EnvironmentFn = unsafe extern "C" fn(cmd: c_uint, data: *mut c_void) -> bool;
pub type VideoRefreshFn = unsafe extern "C" fn(data: *const c_void, width: c_uint, height: c_uint, pitch: usize);
pub type AudioSampleFn = unsafe extern "C" fn(left: i16, right: i16);
pub type AudioSampleBatchFn = unsafe extern "C" fn(data: *const i16, frames: usize) -> usize;
pub type InputPollFn = unsafe extern "C" fn();
pub type InputStateFn = unsafe extern "C" fn(port: c_uint, device: c_uint, index: c_uint, id: c_uint) -> i16;
pub type KeyboardEventFn = unsafe extern "C" fn(down: bool, keycode: c_uint, character: u32, key_modifiers: u16);

#[repr(C)]
pub struct KeyboardCallback {
    pub callback: KeyboardEventFn,
}

#[repr(C)]
pub struct SystemInfo {
    pub library_name: *const c_char,
    pub library_version: *const c_char,
    pub valid_extensions: *const c_char,
    pub need_fullpath: bool,
    pub block_extract: bool,
}

#[repr(C)]
pub struct GameGeometry {
    pub base_width: c_uint,
    pub base_height: c_uint,
    pub max_width: c_uint,
    pub max_height: c_uint,
    pub aspect_ratio: f32,
}

#[repr(C)]
pub struct SystemTiming {
    pub fps: f64,
    pub sample_rate: f64,
}

#[repr(C)]
pub struct SystemAvInfo {
    pub geometry: GameGeometry,
    pub timing: SystemTiming,
}

#[repr(C)]
pub struct GameInfo {
    pub path: *const c_char,
    pub data: *const c_void,
    pub size: usize,
    pub meta: *const c_char,
}

#[repr(C)]
pub struct Variable {
    pub key: *const c_char,
    pub value: *const c_char,
}

#[repr(C)]
pub struct InputDescriptor {
    pub port: c_uint,
    pub device: c_uint,
    pub index: c_uint,
    pub id: c_uint,
    pub description: *const c_char,
}

// RETROK_* (libretro.h enum retro_key), the ones a Flash game can see.
pub const K_BACKSPACE: u32 = 8;
pub const K_TAB: u32 = 9;
pub const K_RETURN: u32 = 13;
pub const K_PAUSE: u32 = 19;
pub const K_ESCAPE: u32 = 27;
pub const K_SPACE: u32 = 32;
pub const K_DELETE: u32 = 127;
pub const K_KP0: u32 = 256;
pub const K_KP_PERIOD: u32 = 266;
pub const K_KP_DIVIDE: u32 = 267;
pub const K_KP_MULTIPLY: u32 = 268;
pub const K_KP_MINUS: u32 = 269;
pub const K_KP_PLUS: u32 = 270;
pub const K_KP_ENTER: u32 = 271;
pub const K_UP: u32 = 273;
pub const K_DOWN: u32 = 274;
pub const K_RIGHT: u32 = 275;
pub const K_LEFT: u32 = 276;
pub const K_INSERT: u32 = 277;
pub const K_HOME: u32 = 278;
pub const K_END: u32 = 279;
pub const K_PAGEUP: u32 = 280;
pub const K_PAGEDOWN: u32 = 281;
pub const K_F1: u32 = 282;
pub const K_F12: u32 = 293;
pub const K_NUMLOCK: u32 = 300;
pub const K_CAPSLOCK: u32 = 301;
pub const K_RSHIFT: u32 = 303;
pub const K_LSHIFT: u32 = 304;
pub const K_RCTRL: u32 = 305;
pub const K_LCTRL: u32 = 306;
pub const K_RALT: u32 = 307;
pub const K_LALT: u32 = 308;
