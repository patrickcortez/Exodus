use winit::event::ElementState;
use winit::keyboard::{Key, NamedKey, SmolStr};

pub fn translate_key(key: &Key, state: ElementState, modifiers_ctrl: bool) -> Option<Vec<u8>> {
    if state != ElementState::Pressed {
        return None;
    }

    if modifiers_ctrl {
        if let Key::Character(ch) = key {
            let c = ch.chars().next()?;
            if c.is_ascii_alphabetic() {
                let ctrl_code = (c.to_ascii_lowercase() as u8) - b'a' + 1;
                return Some(vec![ctrl_code]);
            }
        }
    }

    match key {
        Key::Character(ch) => Some(ch.as_bytes().to_vec()),
        Key::Named(named) => match named {
            NamedKey::Space => Some(vec![b' ']),
            NamedKey::Enter => Some(vec![b'\n']),
            NamedKey::Backspace => Some(vec![0x7f]),
            NamedKey::Tab => Some(vec![b'\t']),
            NamedKey::Escape => Some(vec![0x1b]),
            NamedKey::ArrowUp => Some(vec![0x1b, b'[', b'A']),
            NamedKey::ArrowDown => Some(vec![0x1b, b'[', b'B']),
            NamedKey::ArrowRight => Some(vec![0x1b, b'[', b'C']),
            NamedKey::ArrowLeft => Some(vec![0x1b, b'[', b'D']),
            NamedKey::Home => Some(vec![0x1b, b'[', b'H']),
            NamedKey::End => Some(vec![0x1b, b'[', b'F']),
            NamedKey::PageUp => Some(vec![0x1b, b'[', b'5', b'~']),
            NamedKey::PageDown => Some(vec![0x1b, b'[', b'6', b'~']),
            NamedKey::Delete => Some(vec![0x1b, b'[', b'3', b'~']),
            NamedKey::Insert => Some(vec![0x1b, b'[', b'2', b'~']),
            _ => None,
        },
        _ => None,
    }
}
