use fontdue::{Font, FontSettings};
use std::collections::HashMap;

pub struct FontCache {
    pub font: Font,
    pub cell_width: u32,
    pub cell_height: u32,
    pub baseline: u32,
    cache: HashMap<(u8, bool), Vec<u8>>,
}

impl FontCache {
    pub fn new(size: f32) -> Self {
        let font_data = include_bytes!("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
        let font = Font::from_bytes(
            font_data as &[u8],
            FontSettings {
                scale: size,
                ..FontSettings::default()
            },
        )
        .expect("Failed to load font");

        let metrics = font.metrics('M', size);
        let cell_width = metrics.advance_width.ceil() as u32;
        let cell_height = (size * 1.4).ceil() as u32;
        let baseline = (size * 1.1).ceil() as u32;

        FontCache {
            font,
            cell_width,
            cell_height,
            baseline,
            cache: HashMap::new(),
        }
    }

    pub fn rasterize(&mut self, ch: u8, bold: bool) -> &[u8] {
        let size = if bold {
            self.font.metrics(ch as char, 16.0).advance_width
        } else {
            0.0
        };
        let _ = size;

        let key = (ch, bold);
        if !self.cache.contains_key(&key) {
            let (metrics, bitmap) = self.font.rasterize(ch as char, 16.0);
            let mut cell_bitmap = vec![0u8; (self.cell_width * self.cell_height) as usize];

            let x_off = 0i32;
            let y_off = (self.baseline as i32) - metrics.height as i32 - metrics.ymin;

            for row in 0..metrics.height {
                for col in 0..metrics.width {
                    let dx = col as i32 + x_off;
                    let dy = row as i32 + y_off;
                    if dx >= 0
                        && dx < self.cell_width as i32
                        && dy >= 0
                        && dy < self.cell_height as i32
                    {
                        let src = row * metrics.width + col;
                        let dst = (dy as u32) * self.cell_width + (dx as u32);
                        cell_bitmap[dst as usize] = bitmap[src];
                    }
                }
            }

            self.cache.insert(key, cell_bitmap);
        }

        &self.cache[&key]
    }
}
