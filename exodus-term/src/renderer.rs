use crate::config::{BRIGHT_COLOR_TABLE, COLOR_TABLE};
use crate::console::{ExconCell, ExconHeader, KernelConsole, ATTR_BG_MASK, ATTR_BG_SHIFT, ATTR_BOLD, ATTR_FG_MASK, FLAG_CURSOR_VISIBLE};
use crate::font::FontCache;

pub struct Renderer {
    pub font: FontCache,
    pub last_dirty_seq: u32,
}

impl Renderer {
    pub fn new(font_size: f32) -> Self {
        Renderer {
            font: FontCache::new(font_size),
            last_dirty_seq: u32::MAX,
        }
    }

    pub fn pixel_size(&self, cols: u16, rows: u16) -> (u32, u32) {
        (
            cols as u32 * self.font.cell_width,
            rows as u32 * self.font.cell_height,
        )
    }

    pub fn needs_redraw(&self, console: &KernelConsole) -> bool {
        let header = console.header();
        header.dirty_seq != self.last_dirty_seq
    }

    pub fn render(&mut self, console: &KernelConsole, framebuf: &mut [u32], fb_width: u32) {
        let header: ExconHeader = console.header();
        let rows = header.rows;
        let cols = header.cols;

        let cw = self.font.cell_width;
        let ch = self.font.cell_height;

        for row in 0..rows {
            for col in 0..cols {
                let cell: ExconCell = console.cell(row, col);

                let fg_idx = (cell.attr & ATTR_FG_MASK) as usize;
                let bg_idx = ((cell.attr & ATTR_BG_MASK) >> ATTR_BG_SHIFT) as usize;
                let bold = cell.attr & ATTR_BOLD != 0;

                let (fr, fg, fb) = if bold {
                    BRIGHT_COLOR_TABLE[fg_idx % 8]
                } else {
                    COLOR_TABLE[fg_idx % 8]
                };
                let (br, bg_c, bb) = COLOR_TABLE[bg_idx % 8];

                let is_cursor = (header.flags & FLAG_CURSOR_VISIBLE) != 0
                    && header.cursor_row == row
                    && header.cursor_col == col;

                let glyph = self.font.rasterize(cell.ch, bold);

                let px = col as u32 * cw;
                let py = row as u32 * ch;

                for gy in 0..ch {
                    for gx in 0..cw {
                        let fbx = px + gx;
                        let fby = py + gy;
                        let fb_idx = (fby * fb_width + fbx) as usize;

                        if fb_idx >= framebuf.len() {
                            continue;
                        }

                        let glyph_idx = (gy * cw + gx) as usize;
                        let alpha = if glyph_idx < glyph.len() {
                            glyph[glyph_idx]
                        } else {
                            0
                        };

                        let (r, g, b) = if is_cursor {
                            let inv_alpha = 255 - alpha;
                            (
                                ((fg as u16 * inv_alpha as u16
                                    + br as u16 * alpha as u16)
                                    / 255) as u8,
                                ((fg as u16 * inv_alpha as u16
                                    + bg_c as u16 * alpha as u16)
                                    / 255) as u8,
                                ((fb as u16 * inv_alpha as u16
                                    + bb as u16 * alpha as u16)
                                    / 255) as u8,
                            )
                        } else {
                            (
                                ((fr as u16 * alpha as u16
                                    + br as u16 * (255 - alpha) as u16)
                                    / 255) as u8,
                                ((fg as u16 * alpha as u16
                                    + bg_c as u16 * (255 - alpha) as u16)
                                    / 255) as u8,
                                ((fb as u16 * alpha as u16
                                    + bb as u16 * (255 - alpha) as u16)
                                    / 255) as u8,
                            )
                        };

                        framebuf[fb_idx] = (r as u32) << 16 | (g as u32) << 8 | b as u32;
                    }
                }
            }
        }

        self.last_dirty_seq = header.dirty_seq;
    }
}
