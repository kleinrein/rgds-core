//! Keys a Flash game can be sent: the pad's buttons by option name, and the on-screen keyboard's RETROK codes.

use ruffle_core::events::{KeyDescriptor, KeyLocation, LogicalKey, NamedKey, PhysicalKey};

use crate::libretro as lr;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Bind {
    Key(KeyDescriptor),
    MouseLeft,
    MouseRight,
    Nothing,
}

fn named(physical: PhysicalKey, key: NamedKey, location: KeyLocation) -> KeyDescriptor {
    KeyDescriptor { physical_key: physical, logical_key: LogicalKey::Named(key), key_location: location }
}

fn character(physical: PhysicalKey, c: char) -> KeyDescriptor {
    KeyDescriptor { physical_key: physical, logical_key: LogicalKey::Character(c), key_location: KeyLocation::Standard }
}

fn letter(c: char) -> Option<KeyDescriptor> {
    use PhysicalKey::*;
    const LETTERS: [PhysicalKey; 26] = [
        KeyA, KeyB, KeyC, KeyD, KeyE, KeyF, KeyG, KeyH, KeyI, KeyJ, KeyK, KeyL, KeyM, KeyN, KeyO, KeyP, KeyQ, KeyR,
        KeyS, KeyT, KeyU, KeyV, KeyW, KeyX, KeyY, KeyZ,
    ];
    const DIGITS: [PhysicalKey; 10] = [Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9];
    let lower = c.to_ascii_lowercase();
    if lower.is_ascii_lowercase() {
        return Some(character(LETTERS[(lower as u8 - b'a') as usize], c));
    }
    if c.is_ascii_digit() {
        return Some(character(DIGITS[(c as u8 - b'0') as usize], c));
    }
    None
}

/// A pad button's option value ("space", "z", "up", "mouse", ...).
pub fn bind_named(name: &str) -> Bind {
    use PhysicalKey as P;
    let k = match name {
        "none" | "" => return Bind::Nothing,
        "mouse" | "click" => return Bind::MouseLeft,
        "right click" => return Bind::MouseRight,
        "space" => character(P::Space, ' '),
        "enter" => named(P::Enter, NamedKey::Enter, KeyLocation::Standard),
        "shift" => named(P::ShiftLeft, NamedKey::Shift, KeyLocation::Left),
        "ctrl" => named(P::ControlLeft, NamedKey::Control, KeyLocation::Left),
        "alt" => named(P::AltLeft, NamedKey::Alt, KeyLocation::Left),
        "escape" => named(P::Escape, NamedKey::Escape, KeyLocation::Standard),
        "tab" => named(P::Tab, NamedKey::Tab, KeyLocation::Standard),
        "backspace" => named(P::Backspace, NamedKey::Backspace, KeyLocation::Standard),
        "up" => named(P::ArrowUp, NamedKey::ArrowUp, KeyLocation::Standard),
        "down" => named(P::ArrowDown, NamedKey::ArrowDown, KeyLocation::Standard),
        "left" => named(P::ArrowLeft, NamedKey::ArrowLeft, KeyLocation::Standard),
        "right" => named(P::ArrowRight, NamedKey::ArrowRight, KeyLocation::Standard),
        s if s.chars().count() == 1 => match letter(s.chars().next().unwrap()) {
            Some(k) => k,
            None => return Bind::Nothing,
        },
        _ => return Bind::Nothing,
    };
    Bind::Key(k)
}

/// The on-screen keyboard's key (libretro's RETROK) and the character it types.
pub fn retro_key(code: u32, typed: u32) -> Option<KeyDescriptor> {
    use PhysicalKey as P;
    let typed = char::from_u32(typed).filter(|c| !c.is_control());
    let std = KeyLocation::Standard;
    let k = match code {
        lr::K_BACKSPACE => named(P::Backspace, NamedKey::Backspace, std),
        lr::K_TAB => named(P::Tab, NamedKey::Tab, std),
        lr::K_RETURN => named(P::Enter, NamedKey::Enter, std),
        lr::K_PAUSE => named(P::Pause, NamedKey::Pause, std),
        lr::K_ESCAPE => named(P::Escape, NamedKey::Escape, std),
        lr::K_SPACE => character(P::Space, ' '),
        lr::K_DELETE => named(P::Delete, NamedKey::Delete, std),
        lr::K_UP => named(P::ArrowUp, NamedKey::ArrowUp, std),
        lr::K_DOWN => named(P::ArrowDown, NamedKey::ArrowDown, std),
        lr::K_LEFT => named(P::ArrowLeft, NamedKey::ArrowLeft, std),
        lr::K_RIGHT => named(P::ArrowRight, NamedKey::ArrowRight, std),
        lr::K_INSERT => named(P::Insert, NamedKey::Insert, std),
        lr::K_HOME => named(P::Home, NamedKey::Home, std),
        lr::K_END => named(P::End, NamedKey::End, std),
        lr::K_PAGEUP => named(P::PageUp, NamedKey::PageUp, std),
        lr::K_PAGEDOWN => named(P::PageDown, NamedKey::PageDown, std),
        lr::K_CAPSLOCK => named(P::CapsLock, NamedKey::CapsLock, std),
        lr::K_NUMLOCK => named(P::NumLock, NamedKey::NumLock, std),
        lr::K_LSHIFT => named(P::ShiftLeft, NamedKey::Shift, KeyLocation::Left),
        lr::K_RSHIFT => named(P::ShiftRight, NamedKey::Shift, KeyLocation::Right),
        lr::K_LCTRL => named(P::ControlLeft, NamedKey::Control, KeyLocation::Left),
        lr::K_RCTRL => named(P::ControlRight, NamedKey::Control, KeyLocation::Right),
        lr::K_LALT => named(P::AltLeft, NamedKey::Alt, KeyLocation::Left),
        lr::K_RALT => named(P::AltRight, NamedKey::Alt, KeyLocation::Right),
        lr::K_KP_ENTER => named(P::NumpadEnter, NamedKey::Enter, KeyLocation::Numpad),
        c if (lr::K_F1..=lr::K_F12).contains(&c) => {
            const F: [(PhysicalKey, NamedKey); 12] = [
                (P::F1, NamedKey::F1),
                (P::F2, NamedKey::F2),
                (P::F3, NamedKey::F3),
                (P::F4, NamedKey::F4),
                (P::F5, NamedKey::F5),
                (P::F6, NamedKey::F6),
                (P::F7, NamedKey::F7),
                (P::F8, NamedKey::F8),
                (P::F9, NamedKey::F9),
                (P::F10, NamedKey::F10),
                (P::F11, NamedKey::F11),
                (P::F12, NamedKey::F12),
            ];
            let (p, n) = F[(c - lr::K_F1) as usize];
            named(p, n, std)
        }
        c if (b'a' as u32..=b'z' as u32).contains(&c) || (b'0' as u32..=b'9' as u32).contains(&c) => {
            let mut k = letter(char::from_u32(c)?)?;
            if let Some(t) = typed {
                k.logical_key = LogicalKey::Character(t);
            }
            k
        }
        _ => {
            let physical = match code as u8 {
                b'-' => P::Minus,
                b'=' => P::Equal,
                b'[' => P::BracketLeft,
                b']' => P::BracketRight,
                b'\\' => P::Backslash,
                b';' => P::Semicolon,
                b'\'' => P::Quote,
                b',' => P::Comma,
                b'.' => P::Period,
                b'/' => P::Slash,
                b'`' => P::Backquote,
                _ => P::Unknown,
            };
            let c = typed.or_else(|| char::from_u32(code).filter(|c| c.is_ascii_graphic()))?;
            character(physical, c)
        }
    };
    Some(k)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn binds() {
        assert!(matches!(bind_named("space"), Bind::Key(k) if k.physical_key == PhysicalKey::Space));
        assert!(matches!(bind_named("Z".to_lowercase().as_str()), Bind::Key(k) if k.physical_key == PhysicalKey::KeyZ));
        assert_eq!(bind_named("mouse"), Bind::MouseLeft);
        assert_eq!(bind_named("nonsense"), Bind::Nothing);
    }

    #[test]
    fn typed() {
        let k = retro_key(b'a' as u32, b'A' as u32).unwrap();
        assert_eq!(k.logical_key, LogicalKey::Character('A'));
        assert!(retro_key(lr::K_F1 + 4, 0).is_some());
    }
}
