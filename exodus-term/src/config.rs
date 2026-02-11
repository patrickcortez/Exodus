pub struct Config {
    pub font_size: f32,
    pub default_rows: u16,
    pub default_cols: u16,
    pub cursor_blink_ms: u64,
    pub poll_interval_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            font_size: 16.0,
            default_rows: 40,
            default_cols: 120,
            cursor_blink_ms: 500,
            poll_interval_ms: 16,
        }
    }
}

pub const COLOR_TABLE: [(u8, u8, u8); 8] = [
    (0, 0, 0),
    (170, 0, 0),
    (0, 170, 0),
    (170, 85, 0),
    (0, 0, 170),
    (170, 0, 170),
    (0, 170, 170),
    (170, 170, 170),
];

pub const BRIGHT_COLOR_TABLE: [(u8, u8, u8); 8] = [
    (85, 85, 85),
    (255, 85, 85),
    (85, 255, 85),
    (255, 255, 85),
    (85, 85, 255),
    (255, 85, 255),
    (85, 255, 255),
    (255, 255, 255),
];
