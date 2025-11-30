/*
 * cartridge.c - A Standalone Game Boy Emulator
 *
 * A single-file, C-based emulator for the original Game Boy (DMG).
 * It uses the SDL2 library for windowing, rendering, and input.
 * The display is intentionally filtered to a monochrome green palette.
 *
 * NEW: Now supports MBC1, MBC2, MBC3 (with RTC), and MBC5 controllers,
 * automatic game saving (.sav files), and the original boot ROM.
 *
 * How to Compile:
 * gcc cartridge.c -o cartridge -lSDL2 -O3 -std=c99
 *
 * How to Run:
 * 1. Place the Game Boy boot ROM file named "dmg_boot.bin" in the same
 * directory as the emulator executable.
 * 2. Run the emulator with a game ROM:
 * ./cartridge <path_to_rom.gb>
 *
 * Game progress will be saved to a <rom_name>.sav file automatically on exit.
 *
 * Controls:
 * - D-Pad: Arrow Keys
 * - A:     Z
 * - B:     X
 * - Start: Enter
 * - Select: Right Shift
 * - Exit:  Escape Key
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <SDL2/SDL.h>

// --- Constants ---
#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 144
#define CLOCK_SPEED 4194304
#define CYCLES_PER_FRAME 70224

// --- MBC State ---
// This struct holds all the state for the memory bank controller.
typedef struct {
    int type;
    bool has_battery;
    bool ram_enabled;
    int mode; // For MBC1

    int rom_bank;
    int ram_bank;
    
    uint32_t rom_size;
    uint32_t ram_size;

    // MBC3 Real-Time Clock (RTC) Registers
    struct {
        uint8_t s;  // Seconds
        uint8_t m;  // Minutes
        uint8_t h;  // Hours
        uint16_t d; // Day Counter (lower 9 bits)
        uint8_t latch_reg;
        time_t base_time;
    } rtc;

} MBCState;


// --- Emulator State Structure ---
typedef struct {
    // CPU Registers
    struct {
        uint8_t a, f; uint8_t b, c; uint8_t d, e; uint8_t h, l;
        uint16_t sp; uint16_t pc;
    } reg;

    // Memory
    struct {
        uint8_t boot_rom[256];
        uint8_t* rom;    // Cartridge ROM (dynamic size)
        uint8_t vram[0x2000];
        uint8_t* eram;   // External RAM (dynamic size)
        uint8_t wram[0x2000];
        uint8_t oam[0xA0];
        uint8_t io[0x80];
        uint8_t hram[0x7F];
        uint8_t ie;
    } mem;

    // Memory Bank Controller
    MBCState mbc;

    // PPU State
    struct {
        int mode; int mode_clock; int line;
        uint32_t framebuffer[SCREEN_WIDTH * SCREEN_HEIGHT];
    } ppu;

    // Timer State
    struct {
        int div_counter; int tima_counter;
    } timer;

    // Joypad State
    uint8_t joypad_state;
    uint8_t joypad_select;

    // Control
    bool halted;
    bool ime;
    bool boot_rom_active;
    char save_path[1024];
    
} GameBoy;

// --- Global Context ---
GameBoy gb;
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
bool is_running = true;

// --- Function Prototypes ---
void gb_init();
void gb_load_boot_rom(const char* path);
void gb_load_rom(const char* path);
void gb_load_sram();
void gb_save_sram();
void gb_run_frame();
int execute_opcode();
uint8_t rb(uint16_t addr);
void wb(uint16_t addr, uint8_t value);
void ppu_step(int cycles);
void render_scanline();
void handle_interrupts();
void request_interrupt(int id);
void timer_step(int cycles);
void handle_input(SDL_Event* event);
void rtc_latch();
void rtc_update();


// --- Memory Access ---
uint8_t rb(uint16_t addr) {
    // Boot ROM active?
    if (gb.boot_rom_active && addr < 0x0100) {
        return gb.mem.boot_rom[addr];
    }

if (addr <= 0x7FFF) { // ROM area
    if (gb.mbc.type == 0) { // No MBC
        return gb.mem.rom[addr];
    }

    // Handle Bank 00 Area (0x0000 - 0x3FFF)
    if (addr <= 0x3FFF) {
        // For MBC1 in mode 1, the upper bits of the ROM bank are used here.
        if (gb.mbc.type == 1 && gb.mbc.mode == 1) {
            uint32_t bank = gb.mbc.ram_bank << 5;
            uint32_t offset = (uint32_t)bank * 0x4000 + addr;
            return gb.mem.rom[offset % gb.mbc.rom_size];
        }
        // Otherwise, it's always bank 0.
        return gb.mem.rom[addr];
    }

    // Handle Switchable Bank Area (0x4000 - 0x7FFF)
    if (addr <= 0x7FFF) {
        uint32_t offset = (uint32_t)gb.mbc.rom_bank * 0x4000 + (addr - 0x4000);
        return gb.mem.rom[offset % gb.mbc.rom_size];
    }
}
    
    // External RAM
    if (addr >= 0xA000 && addr <= 0xBFFF) {
        if (!gb.mbc.ram_enabled || !gb.mem.eram) return 0xFF;

        if (gb.mbc.type == 3 && gb.mbc.ram_bank >= 0x08) { // MBC3 RTC Read
            switch(gb.mbc.ram_bank) {
                case 0x08: return gb.mbc.rtc.s;
                case 0x09: return gb.mbc.rtc.m;
                case 0x0A: return gb.mbc.rtc.h;
                case 0x0B: return gb.mbc.rtc.d & 0xFF;
                case 0x0C: return (gb.mbc.rtc.d >> 8) & 0xFF;
            }
        }
        
        uint32_t offset = (uint32_t)gb.mbc.ram_bank * 0x2000 + (addr - 0xA000);
        return gb.mem.eram[offset % gb.mbc.ram_size];
    }

    // Standard Memory Map
    if (addr < 0xA000) return gb.mem.vram[addr - 0x8000];
    if (addr < 0xC000) return 0xFF; // Should have been handled by ERAM
    if (addr < 0xE000) return gb.mem.wram[addr - 0xC000];
    if (addr < 0xFE00) return gb.mem.wram[addr - 0xE000]; // Echo RAM
    if (addr < 0xFEA0) return gb.mem.oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0; // Unusable memory
    if (addr < 0xFF80) { // I/O Registers

if (addr == 0xFF00) { // Joypad
    // Bits 7-6 should be 1. Keep bits 5-4 exactly as the last write (select lines).
    uint8_t result = 0xC0 | (gb.joypad_select & 0x30);

    // If bit4 (0x10) is LOW -> D-Pad group selected -> return lower 4 bits (0..3).
    if ((gb.joypad_select & 0x10) == 0) {
        result |= (gb.joypad_state & 0x0F);
    }
    // Else if bit5 (0x20) is LOW -> Action group selected -> return upper 4 bits shifted down.
    else if ((gb.joypad_select & 0x20) == 0) {
        result |= ((gb.joypad_state >> 4) & 0x0F);
    }
    // If neither group is selected, bits 3-0 should read as 1.
    else {
        result |= 0x0F;
    }

    return result;
}


        return gb.mem.io[addr - 0xFF00];
    }
    if (addr < 0xFFFF) return gb.mem.hram[addr - 0xFF80];
    return gb.mem.ie;
}

void wb(uint16_t addr, uint8_t value) {
    // MBC Control Registers
    if (addr <= 0x7FFF) {
        switch (gb.mbc.type) {
            case 1: // MBC1
                if (addr <= 0x1FFF) { // RAM Enable
                    gb.mbc.ram_enabled = (value & 0x0F) == 0x0A;
                } else if (addr <= 0x3FFF) { // ROM Bank (lower 5 bits)
                    uint8_t bank = value & 0x1F; if (bank == 0) bank = 1;
                    gb.mbc.rom_bank = (gb.mbc.rom_bank & 0xE0) | bank;
                } else if (addr <= 0x5FFF) { // RAM Bank or Upper ROM Bank
                    if (gb.mbc.mode == 0) {
                        gb.mbc.rom_bank = (gb.mbc.rom_bank & 0x1F) | ((value & 0x03) << 5);
                    } else {
                        gb.mbc.ram_bank = value & 0x03;
                    }
                } else if (addr <= 0x7FFF) { // Mode Select
                    gb.mbc.mode = value & 0x01;
                }
                break;
            case 2: // MBC2
                if (addr <= 0x3FFF) {
                    if (addr & 0x0100) { // ROM Bank Select
                         gb.mbc.rom_bank = value & 0x0F;
                    } else { // RAM Enable
                        gb.mbc.ram_enabled = (value & 0x0F) == 0x0A;
                    }
                }
                break;
            case 3: // MBC3
                 if (addr <= 0x1FFF) { // RAM and RTC Enable
                    gb.mbc.ram_enabled = (value & 0x0F) == 0x0A;
                } else if (addr <= 0x3FFF) { // ROM Bank
                    uint8_t bank = value & 0x7F; if (bank == 0) bank = 1;
                    gb.mbc.rom_bank = bank;
                } else if (addr <= 0x5FFF) { // RAM Bank or RTC Register Select
                    gb.mbc.ram_bank = value;
                } else if (addr <= 0x7FFF) { // Latch Clock Data
                    if (gb.mbc.rtc.latch_reg == 0x00 && value == 0x01) rtc_latch();
                    gb.mbc.rtc.latch_reg = value;
                }
                break;
            case 5: // MBC5
                if (addr <= 0x1FFF) { // RAM Enable
                    gb.mbc.ram_enabled = (value & 0x0F) == 0x0A;
                } else if (addr <= 0x2FFF) { // ROM Bank (lower 8 bits)
                    gb.mbc.rom_bank = (gb.mbc.rom_bank & 0x100) | value;
                } else if (addr <= 0x3FFF) { // ROM Bank (9th bit)
                    gb.mbc.rom_bank = (gb.mbc.rom_bank & 0xFF) | ((value & 0x01) << 8);
                } else if (addr <= 0x5FFF) { // RAM Bank
                    gb.mbc.ram_bank = value & 0x0F;
                }
                break;
        }
        // Fix ROM bank if it's out of bounds for the cartridge size
        int rom_bank_mask = (gb.mbc.rom_size / 0x4000) - 1;
        gb.mbc.rom_bank &= rom_bank_mask;
        if (gb.mbc.rom_bank == 0 && gb.mbc.type != 2) gb.mbc.rom_bank = 1;

        return;
    }

    // External RAM
    if (addr >= 0xA000 && addr <= 0xBFFF) {
        if (!gb.mbc.ram_enabled || !gb.mem.eram) return;
        
        if (gb.mbc.type == 2) { // MBC2 has 4-bit RAM
            uint32_t offset = (addr - 0xA000) % 512;
            gb.mem.eram[offset] = value & 0x0F;
            return;
        }
        
        if (gb.mbc.type == 3 && gb.mbc.ram_bank >= 0x08) { // MBC3 RTC Write
            rtc_update(); // Make sure our internal clock is up to date before changing it
            switch(gb.mbc.ram_bank) {
                case 0x08: gb.mbc.rtc.s = value; break;
                case 0x09: gb.mbc.rtc.m = value; break;
                case 0x0A: gb.mbc.rtc.h = value; break;
                case 0x0B: gb.mbc.rtc.d = (gb.mbc.rtc.d & 0xFF00) | value; break;
                case 0x0C: gb.mbc.rtc.d = (gb.mbc.rtc.d & 0x00FF) | ((uint16_t)value << 8); break;
            }
            gb.mbc.rtc.base_time = time(NULL); // Reset base time after a manual write
            return;
        }
        
        uint32_t offset = (uint32_t)gb.mbc.ram_bank * 0x2000 + (addr - 0xA000);
        gb.mem.eram[offset % gb.mbc.ram_size] = value;
        return;
    }
    
    // Standard Memory Map
    if (addr < 0xA000) gb.mem.vram[addr - 0x8000] = value;
    else if (addr < 0xE000) gb.mem.wram[addr - 0xC000] = value;
    else if (addr < 0xFE00) gb.mem.wram[addr - 0xE000] = value;
    else if (addr < 0xFEA0) gb.mem.oam[addr - 0xFE00] = value;
    else if (addr < 0xFF00) return;
    else if (addr < 0xFF80) { // I/O Registers
        if (addr == 0xFF04) { gb.mem.io[addr-0xFF00] = 0; gb.timer.div_counter = 0; } 
        else if (addr == 0xFF00) { gb.joypad_select = value & 0x30; }
        else if (addr == 0xFF46) {
            uint16_t src = value << 8; for (int i = 0; i < 0xA0; i++) wb(0xFE00 + i, rb(src + i));
        } else if (addr == 0xFF50 && value == 1) { // Disable Boot ROM
            gb.boot_rom_active = false;
        }
        else { gb.mem.io[addr - 0xFF00] = value; }
    } 
    else if (addr < 0xFFFF) { gb.mem.hram[addr - 0xFF80] = value; } 
    else { gb.mem.ie = value; }
}

// --- RTC Logic ---
void rtc_latch() {
    time_t current_time = time(NULL);
    if (gb.mbc.rtc.base_time == 0) gb.mbc.rtc.base_time = current_time;
    
    // Calculate elapsed time and update registers
    long elapsed_seconds = (long)(current_time - gb.mbc.rtc.base_time);

    // This is a simplified update. A full implementation would handle day/month/year rollovers.
    // For now, we add elapsed time to the stored values.
    elapsed_seconds += gb.mbc.rtc.s;
    gb.mbc.rtc.s = elapsed_seconds % 60;
    long elapsed_minutes = elapsed_seconds / 60;
    elapsed_minutes += gb.mbc.rtc.m;
    gb.mbc.rtc.m = elapsed_minutes % 60;
    long elapsed_hours = elapsed_minutes / 60;
    elapsed_hours += gb.mbc.rtc.h;
    gb.mbc.rtc.h = elapsed_hours % 24;
    long elapsed_days = elapsed_hours / 24;
    gb.mbc.rtc.d += elapsed_days;
    
    gb.mbc.rtc.base_time = current_time; // Reset base time
}

void rtc_update() {
    rtc_latch(); // Just reuse latch logic for updating internal state
}

// --- PPU ---
void ppu_step(int cycles) {
    gb.ppu.mode_clock += cycles;
    uint8_t lcdc = rb(0xFF40);
    uint8_t stat = rb(0xFF41);

    if (!(lcdc & 0x80)) {
        gb.ppu.mode_clock = 0;
        gb.ppu.line = 0;
        wb(0xFF41, (stat & 0xFC) | 0);
        return;
    }

    switch (gb.ppu.mode) {
        case 2:
            if (gb.ppu.mode_clock >= 80) {
                gb.ppu.mode_clock -= 80;
                gb.ppu.mode = 3; wb(0xFF41, (stat & 0xFC) | 3);
            }
            break;
        case 3:
            if (gb.ppu.mode_clock >= 172) {
                gb.ppu.mode_clock -= 172;
                gb.ppu.mode = 0; wb(0xFF41, (stat & 0xFC) | 0);
                render_scanline();
                if (stat & 0x08) request_interrupt(1);
            }
            break;
        case 0:
            if (gb.ppu.mode_clock >= 204) {
                gb.ppu.mode_clock -= 204;
                gb.ppu.line++; wb(0xFF44, gb.ppu.line);
                if (gb.ppu.line == 144) {
                    gb.ppu.mode = 1; wb(0xFF41, (stat & 0xFC) | 1);
                    request_interrupt(0);
                    if (stat & 0x10) request_interrupt(1);
                } else {
                    gb.ppu.mode = 2; wb(0xFF41, (stat & 0xFC) | 2);
                    if (stat & 0x20) request_interrupt(1);
                }
            }
            break;
        case 1:
            if (gb.ppu.mode_clock >= 456) {
                gb.ppu.mode_clock -= 456;
                gb.ppu.line++; wb(0xFF44, gb.ppu.line);
                if (gb.ppu.line > 153) {
                    gb.ppu.line = 0; wb(0xFF44, 0);
                    gb.ppu.mode = 2; wb(0xFF41, (stat & 0xFC) | 2);
                    if (stat & 0x20) request_interrupt(1);
                }
            }
            break;
    }
    
    if(gb.ppu.line == rb(0xFF45)) {
        wb(0xFF41, stat | 0x04);
        if (stat & 0x40) request_interrupt(1);
    } else {
        wb(0xFF41, stat & ~0x04);
    }
}
void render_scanline() {
    uint8_t lcdc = rb(0xFF40); if (!(lcdc & 0x80)) return;
    uint8_t palette[4] = {0, 1, 2, 3}; uint8_t bgp = rb(0xFF47);
    palette[0] = (bgp >> 0) & 0x3; palette[1] = (bgp >> 2) & 0x3;
    palette[2] = (bgp >> 4) & 0x3; palette[3] = (bgp >> 6) & 0x3;
    if (lcdc & 0x01) {
        uint16_t tile_map = (lcdc & 0x08) ? 0x9C00 : 0x9800;
        uint16_t tile_data = (lcdc & 0x10) ? 0x8000 : 0x9000;
        bool signed_addressing = !(lcdc & 0x10);
        uint8_t scy = rb(0xFF42); uint8_t scx = rb(0xFF43);
        uint8_t y_pos = (gb.ppu.line + scy) & 0xFF;
        for (int i = 0; i < SCREEN_WIDTH; i++) {
            uint8_t x_pos = (i + scx) & 0xFF;
            uint16_t map_addr = tile_map + (y_pos / 8) * 32 + (x_pos / 8);
            uint8_t tile_idx = rb(map_addr);
            uint16_t tile_addr;
            if (signed_addressing) tile_addr = tile_data + ((int8_t)tile_idx * 16);
            else tile_addr = tile_data + tile_idx * 16;
            uint8_t byte1 = rb(tile_addr + (y_pos % 8) * 2);
            uint8_t byte2 = rb(tile_addr + (y_pos % 8) * 2 + 1);
            int bit = 7 - (x_pos % 8);
            uint8_t color_idx = ((byte2 >> bit) & 1) << 1 | ((byte1 >> bit) & 1);
            uint32_t color = 0;
            switch(palette[color_idx]) {
                case 0: color = 0xFF102D10; break; case 1: color = 0xFF1E6E1E; break;
                case 2: color = 0xFF33CD33; break; case 3: color = 0xFF8CFF8C; break;
            }
            gb.ppu.framebuffer[gb.ppu.line * SCREEN_WIDTH + i] = color;
        }
    }
    
    if (lcdc & 0x02) {
    for (int s = 0; s < 40; s++) {
        uint8_t sprite_y = rb(0xFE00 + s * 4);
        uint8_t sprite_x = rb(0xFE00 + s * 4 + 1);
        uint8_t tile_idx  = rb(0xFE00 + s * 4 + 2);
        uint8_t attr      = rb(0xFE00 + s * 4 + 3);

        int sprite_size = (lcdc & 0x04) ? 16 : 8;
        int top = sprite_y - 16;
        int left = sprite_x - 8;

        if (gb.ppu.line < top || gb.ppu.line >= top + sprite_size) continue;

        int line_in_sprite = gb.ppu.line - top;
        // Vertical flip (bit 6 = 0x40)
        if (attr & 0x40) line_in_sprite = sprite_size - 1 - line_in_sprite;

        // For 8x16 sprites the hardware forces the tile index LSB to 0.
        uint8_t base_tile = (sprite_size == 16) ? (tile_idx & 0xFE) : tile_idx;
        uint16_t tile_base_addr = 0x8000 + (base_tile * 16);
        uint16_t line_addr = tile_base_addr + (line_in_sprite * 2);

        uint8_t byte1 = rb(line_addr);       // lower bitplane
        uint8_t byte2 = rb(line_addr + 1);   // upper bitplane

        for (int bit = 0; bit < 8; bit++) {
            int x_pos = left + bit;
            if (x_pos < 0 || x_pos >= SCREEN_WIDTH) continue;

            // Compute bit index inside the tile bytes.
            // tile bit 7 = leftmost pixel, tile bit 0 = rightmost pixel.
            // If X flip (bit 5 = 0x20) is set we invert the bit index.
            int bit_index = 7 - bit;
            if (attr & 0x20) bit_index = bit;

            uint8_t color_idx = ((byte2 >> bit_index) & 1) << 1 | ((byte1 >> bit_index) & 1);
            if (color_idx == 0) continue; // transparent

            // Palette selection: attr bit 4 chooses OBP1/OBP0
            uint8_t obp = (attr & 0x10) ? rb(0xFF49) : rb(0xFF48);
            uint8_t final_color = (obp >> (color_idx * 2)) & 0x3;

            // Priority: if bit 7 is set, sprite is behind non-zero BG colors.
            if ((attr & 0x80) && (gb.ppu.framebuffer[gb.ppu.line * SCREEN_WIDTH + x_pos] != 0xFF102D10)) {
                continue;
            }

            uint32_t color = 0;
            switch(final_color) {
                case 0: color = 0xFF0F380F; break;
                case 1: color = 0xFF306230; break;
                case 2: color = 0xFF8BAC0F; break;
                case 3: color = 0xFF9BBC0F; break;
            }
            gb.ppu.framebuffer[gb.ppu.line * SCREEN_WIDTH + x_pos] = color;
        }
    }
}

}

// --- Interrupts ---
void request_interrupt(int id) {
    uint8_t if_reg = rb(0xFF0F); wb(0xFF0F, if_reg | (1 << id));
}
void handle_interrupts() {
    uint8_t ie = gb.mem.ie; uint8_t if_reg = rb(0xFF0F);
    if (gb.halted && (if_reg & ie)) gb.halted = false;
    if (!gb.ime) return;
    uint8_t fired = ie & if_reg;
    if (fired) {
        gb.halted = false;
        for (int i = 0; i < 5; i++) {
            if (fired & (1 << i)) {
                gb.ime = false; wb(0xFF0F, if_reg & ~(1 << i));
                gb.reg.sp -= 2; wb(gb.reg.sp + 1, (gb.reg.pc >> 8) & 0xFF); wb(gb.reg.sp, gb.reg.pc & 0xFF);
                switch (i) {
                    case 0: gb.reg.pc = 0x40; break; case 1: gb.reg.pc = 0x48; break;
                    case 2: gb.reg.pc = 0x50; break; case 3: gb.reg.pc = 0x58; break;
                    case 4: gb.reg.pc = 0x60; break;
                }
                return;
            }
        }
    }
}

// --- Timers ---
void timer_step(int cycles) {
    gb.timer.div_counter += cycles;
    if (gb.timer.div_counter >= 256) {
        gb.timer.div_counter -= 256; gb.mem.io[0x04]++;
    }
    uint8_t tac = rb(0xFF07);
    if (tac & 0x04) {
        gb.timer.tima_counter += cycles;
        int freq = 0;
        switch (tac & 0x03) {
            case 0: freq = 1024; break; case 1: freq = 16; break;
            case 2: freq = 64; break; case 3: freq = 256; break;
        }
        while (gb.timer.tima_counter >= freq) {
            gb.timer.tima_counter -= freq;
            uint8_t tima = rb(0xFF05);
            if (tima == 0xFF) { wb(0xFF05, rb(0xFF06)); request_interrupt(2); } 
            else { wb(0xFF05, tima + 1); }
        }
    }
}

// --- Main CPU execution logic ---
// This is intentionally left in its original form as the core logic is sound.
// All MBC and memory changes are handled in rb() and wb().
int execute_opcode() {
    uint8_t opcode = rb(gb.reg.pc++); int cycles = 4;
    #define REG_A gb.reg.a
    #define REG_B gb.reg.b
    #define REG_C gb.reg.c
    #define REG_D gb.reg.d
    #define REG_E gb.reg.e
    #define REG_H gb.reg.h
    #define REG_L gb.reg.l
    #define REG_F gb.reg.f
    #define REG_PC gb.reg.pc
    #define REG_SP gb.reg.sp
    #define SET_Z(v) (REG_F = (v) ? (REG_F | 0x80) : (REG_F & ~0x80))
    #define SET_N(v) (REG_F = (v) ? (REG_F | 0x40) : (REG_F & ~0x40))
    #define SET_H(v) (REG_F = (v) ? (REG_F | 0x20) : (REG_F & ~0x20))
    #define SET_C(v) (REG_F = (v) ? (REG_F | 0x10) : (REG_F & ~0x10))
    #define GET_Z() ((REG_F >> 7) & 1)
    #define GET_N() ((REG_F >> 6) & 1)
    #define GET_H() ((REG_F >> 5) & 1)
    #define GET_C() ((REG_F >> 4) & 1)
    #define REG_BC ((uint16_t)REG_B << 8 | REG_C)
    #define REG_DE ((uint16_t)REG_D << 8 | REG_E)
    #define REG_HL ((uint16_t)REG_H << 8 | REG_L)
    #define SET_BC(v) do { REG_B = (v) >> 8; REG_C = (v) & 0xFF; } while(0)
    #define SET_DE(v) do { REG_D = (v) >> 8; REG_E = (v) & 0xFF; } while(0)
    #define SET_HL(v) do { REG_H = (v) >> 8; REG_L = (v) & 0xFF; } while(0)
    void ADD(uint8_t val) { uint16_t r=REG_A+val; SET_Z((r&0xFF)==0); SET_N(0); SET_H((REG_A&0x0F)+(val&0x0F)>0x0F); SET_C(r>0xFF); REG_A=r; }
    void ADC(uint8_t val) { uint16_t r=REG_A+val+GET_C(); SET_Z((r&0xFF)==0); SET_N(0); SET_H((REG_A&0x0F)+(val&0x0F)+GET_C()>0x0F); SET_C(r>0xFF); REG_A=r; }
    void SUB(uint8_t val) { uint16_t r=REG_A-val; SET_Z((r&0xFF)==0); SET_N(1); SET_H((REG_A&0x0F)<(val&0x0F)); SET_C(REG_A<val); REG_A=r; }
    void SBC(uint8_t val) { uint16_t r=REG_A-val-GET_C(); SET_Z((r&0xFF)==0); SET_N(1); SET_H((REG_A&0x0F)<((val&0x0F)+GET_C())); SET_C(REG_A<(val+GET_C())); REG_A=r; }
    void AND(uint8_t val) { REG_A&=val; SET_Z(REG_A==0); SET_N(0); SET_H(1); SET_C(0); }
    void OR(uint8_t val)  { REG_A|=val; SET_Z(REG_A==0); SET_N(0); SET_H(0); SET_C(0); }
    void XOR(uint8_t val) { REG_A^=val; SET_Z(REG_A==0); SET_N(0); SET_H(0); SET_C(0); }
    void CP(uint8_t val) { SET_Z(REG_A==val); SET_N(1); SET_H((REG_A&0x0F)<(val&0x0F)); SET_C(REG_A<val); }
    void INC(uint8_t* r) { (*r)++; SET_Z(*r==0); SET_N(0); SET_H((*r&0x0F)==0); }
    void DEC(uint8_t* r) { (*r)--; SET_Z(*r==0); SET_N(1); SET_H((*r&0x0F)==0x0F); }
    switch(opcode){
        case 0x00: break; case 0x01: SET_BC(rb(REG_PC)|(rb(REG_PC+1)<<8)); REG_PC+=2; cycles=12; break;
        case 0x02: wb(REG_BC,REG_A); cycles=8; break; case 0x03: SET_BC(REG_BC+1); cycles=8; break;
        case 0x04: INC(&REG_B); break; case 0x05: DEC(&REG_B); break; case 0x06: REG_B=rb(REG_PC++); cycles=8; break;
        case 0x07: {uint8_t c=REG_A>>7; REG_A=(REG_A<<1)|c; SET_Z(0); SET_N(0); SET_H(0); SET_C(c); break;}
        case 0x08: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; wb(a,REG_SP&0xFF); wb(a+1,REG_SP>>8); cycles=20; break;}
        case 0x09: {uint32_t r=REG_HL+REG_BC; SET_N(0); SET_H((REG_HL&0xFFF)+(REG_BC&0xFFF)>0xFFF); SET_C(r>0xFFFF); SET_HL(r); cycles=8; break;}
        case 0x0A: REG_A=rb(REG_BC); cycles=8; break; case 0x0B: SET_BC(REG_BC-1); cycles=8; break;
        case 0x0C: INC(&REG_C); break; case 0x0D: DEC(&REG_C); break; case 0x0E: REG_C=rb(REG_PC++); cycles=8; break;
        case 0x0F: {uint8_t c=REG_A&1; REG_A=(REG_A>>1)|(c<<7); SET_Z(0); SET_N(0); SET_H(0); SET_C(c); break;}
        case 0x10: REG_PC++; break; case 0x11: SET_DE(rb(REG_PC)|(rb(REG_PC+1)<<8)); REG_PC+=2; cycles=12; break;
        case 0x12: wb(REG_DE,REG_A); cycles=8; break; case 0x13: SET_DE(REG_DE+1); cycles=8; break;
        case 0x14: INC(&REG_D); break; case 0x15: DEC(&REG_D); break; case 0x16: REG_D=rb(REG_PC++); cycles=8; break;
        case 0x17: {uint8_t c=REG_A>>7; REG_A=(REG_A<<1)|GET_C(); SET_Z(0); SET_N(0); SET_H(0); SET_C(c); break;}
        case 0x18: {int8_t o=rb(REG_PC++); REG_PC+=o; cycles=12; break;}
        case 0x19: {uint32_t r=REG_HL+REG_DE; SET_N(0); SET_H((REG_HL&0xFFF)+(REG_DE&0xFFF)>0xFFF); SET_C(r>0xFFFF); SET_HL(r); cycles=8; break;}
        case 0x1A: REG_A=rb(REG_DE); cycles=8; break; case 0x1B: SET_DE(REG_DE-1); cycles=8; break;
        case 0x1C: INC(&REG_E); break; case 0x1D: DEC(&REG_E); break; case 0x1E: REG_E=rb(REG_PC++); cycles=8; break;
        case 0x1F: {uint8_t c=REG_A&1; REG_A=(REG_A>>1)|(GET_C()<<7); SET_Z(0); SET_N(0); SET_H(0); SET_C(c); break;}
        case 0x20: {int8_t o=rb(REG_PC++); if(!GET_Z()){REG_PC+=o; cycles=12;}else{cycles=8;} break;}
        case 0x21: SET_HL(rb(REG_PC)|(rb(REG_PC+1)<<8)); REG_PC+=2; cycles=12; break;
        case 0x22: wb(REG_HL,REG_A); SET_HL(REG_HL+1); cycles=8; break; case 0x23: SET_HL(REG_HL+1); cycles=8; break;
        case 0x24: INC(&REG_H); break; case 0x25: DEC(&REG_H); break; case 0x26: REG_H=rb(REG_PC++); cycles=8; break;
        case 0x27: {uint16_t a=REG_A; if(GET_N()){if(GET_H())a=(a-6)&0xFF; if(GET_C())a-=0x60;}else{if(GET_H()||(a&0xF)>9)a+=6; if(GET_C()||a>0x9F)a+=0x60;} if((a&0x100)==0x100)SET_C(1); REG_A=a; SET_Z(REG_A==0); SET_H(0); break;}
        case 0x28: {int8_t o=rb(REG_PC++); if(GET_Z()){REG_PC+=o; cycles=12;}else{cycles=8;} break;}
        case 0x29: {uint32_t r=REG_HL+REG_HL; SET_N(0); SET_H((REG_HL&0xFFF)+(REG_HL&0xFFF)>0xFFF); SET_C(r>0xFFFF); SET_HL(r); cycles=8; break;}
        case 0x2A: REG_A=rb(REG_HL); SET_HL(REG_HL+1); cycles=8; break; case 0x2B: SET_HL(REG_HL-1); cycles=8; break;
        case 0x2C: INC(&REG_L); break; case 0x2D: DEC(&REG_L); break; case 0x2E: REG_L=rb(REG_PC++); cycles=8; break;
        case 0x2F: REG_A=~REG_A; SET_N(1); SET_H(1); break;
        case 0x30: {int8_t o=rb(REG_PC++); if(!GET_C()){REG_PC+=o; cycles=12;}else{cycles=8;} break;}
        case 0x31: REG_SP=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; cycles=12; break;
        case 0x32: wb(REG_HL,REG_A); SET_HL(REG_HL-1); cycles=8; break; case 0x33: REG_SP++; cycles=8; break;
        case 0x34: {uint8_t v=rb(REG_HL); INC(&v); wb(REG_HL,v); cycles=12; break;}
        case 0x35: {uint8_t v=rb(REG_HL); DEC(&v); wb(REG_HL,v); cycles=12; break;}
        case 0x36: wb(REG_HL,rb(REG_PC++)); cycles=12; break;
        case 0x37: SET_N(0); SET_H(0); SET_C(1); break;
        case 0x38: {int8_t o=rb(REG_PC++); if(GET_C()){REG_PC+=o; cycles=12;}else{cycles=8;} break;}
        case 0x39: {uint32_t r=REG_HL+REG_SP; SET_N(0); SET_H((REG_HL&0xFFF)+(REG_SP&0xFFF)>0xFFF); SET_C(r>0xFFFF); SET_HL(r); cycles=8; break;}
        case 0x3A: REG_A=rb(REG_HL); SET_HL(REG_HL-1); cycles=8; break; case 0x3B: REG_SP--; cycles=8; break;
        case 0x3C: INC(&REG_A); break; case 0x3D: DEC(&REG_A); break; case 0x3E: REG_A=rb(REG_PC++); cycles=8; break;
        case 0x3F: SET_N(0); SET_H(0); SET_C(!GET_C()); break;
        case 0x40: break; case 0x41: REG_B=REG_C; break; case 0x42: REG_B=REG_D; break;
        case 0x43: REG_B=REG_E; break; case 0x44: REG_B=REG_H; break; case 0x45: REG_B=REG_L; break;
        case 0x46: REG_B=rb(REG_HL); cycles=8; break; case 0x47: REG_B=REG_A; break;
        case 0x48: REG_C=REG_B; break; case 0x49: break; case 0x4A: REG_C=REG_D; break;
        case 0x4B: REG_C=REG_E; break; case 0x4C: REG_C=REG_H; break; case 0x4D: REG_C=REG_L; break;
        case 0x4E: REG_C=rb(REG_HL); cycles=8; break; case 0x4F: REG_C=REG_A; break;
        case 0x50: REG_D=REG_B; break; case 0x51: REG_D=REG_C; break; case 0x52: break;
        case 0x53: REG_D=REG_E; break; case 0x54: REG_D=REG_H; break; case 0x55: REG_D=REG_L; break;
        case 0x56: REG_D=rb(REG_HL); cycles=8; break; case 0x57: REG_D=REG_A; break;
        case 0x58: REG_E=REG_B; break; case 0x59: REG_E=REG_C; break; case 0x5A: REG_E=REG_D; break;
        case 0x5B: break; case 0x5C: REG_E=REG_H; break; case 0x5D: REG_E=REG_L; break;
        case 0x5E: REG_E=rb(REG_HL); cycles=8; break; case 0x5F: REG_E=REG_A; break;
        case 0x60: REG_H=REG_B; break; case 0x61: REG_H=REG_C; break; case 0x62: REG_H=REG_D; break;
        case 0x63: REG_H=REG_E; break; case 0x64: break; case 0x65: REG_H=REG_L; break;
        case 0x66: REG_H=rb(REG_HL); cycles=8; break; case 0x67: REG_H=REG_A; break;
        case 0x68: REG_L=REG_B; break; case 0x69: REG_L=REG_C; break; case 0x6A: REG_L=REG_D; break;
        case 0x6B: REG_L=REG_E; break; case 0x6C: REG_L=REG_H; break; case 0x6D: break;
        case 0x6E: REG_L=rb(REG_HL); cycles=8; break; case 0x6F: REG_L=REG_A; break;
        case 0x70: wb(REG_HL,REG_B); cycles=8; break; case 0x71: wb(REG_HL,REG_C); cycles=8; break;
        case 0x72: wb(REG_HL,REG_D); cycles=8; break; case 0x73: wb(REG_HL,REG_E); cycles=8; break;
        case 0x74: wb(REG_HL,REG_H); cycles=8; break; case 0x75: wb(REG_HL,REG_L); cycles=8; break;
        case 0x76: gb.halted=true; break; case 0x77: wb(REG_HL,REG_A); cycles=8; break;
        case 0x78: REG_A=REG_B; break; case 0x79: REG_A=REG_C; break; case 0x7A: REG_A=REG_D; break;
        case 0x7B: REG_A=REG_E; break; case 0x7C: REG_A=REG_H; break; case 0x7D: REG_A=REG_L; break;
        case 0x7E: REG_A=rb(REG_HL); cycles=8; break; case 0x7F: break;
        case 0x80: ADD(REG_B); break; case 0x81: ADD(REG_C); break; case 0x82: ADD(REG_D); break; case 0x83: ADD(REG_E); break;
        case 0x84: ADD(REG_H); break; case 0x85: ADD(REG_L); break; case 0x86: ADD(rb(REG_HL)); cycles=8; break; case 0x87: ADD(REG_A); break;
        case 0x88: ADC(REG_B); break; case 0x89: ADC(REG_C); break; case 0x8A: ADC(REG_D); break; case 0x8B: ADC(REG_E); break;
        case 0x8C: ADC(REG_H); break; case 0x8D: ADC(REG_L); break; case 0x8E: ADC(rb(REG_HL)); cycles=8; break; case 0x8F: ADC(REG_A); break;
        case 0x90: SUB(REG_B); break; case 0x91: SUB(REG_C); break; case 0x92: SUB(REG_D); break; case 0x93: SUB(REG_E); break;
        case 0x94: SUB(REG_H); break; case 0x95: SUB(REG_L); break; case 0x96: SUB(rb(REG_HL)); cycles=8; break; case 0x97: SUB(REG_A); break;
        case 0x98: SBC(REG_B); break; case 0x99: SBC(REG_C); break; case 0x9A: SBC(REG_D); break; case 0x9B: SBC(REG_E); break;
        case 0x9C: SBC(REG_H); break; case 0x9D: SBC(REG_L); break; case 0x9E: SBC(rb(REG_HL)); cycles=8; break; case 0x9F: SBC(REG_A); break;
        case 0xA0: AND(REG_B); break; case 0xA1: AND(REG_C); break; case 0xA2: AND(REG_D); break; case 0xA3: AND(REG_E); break;
        case 0xA4: AND(REG_H); break; case 0xA5: AND(REG_L); break; case 0xA6: AND(rb(REG_HL)); cycles=8; break; case 0xA7: AND(REG_A); break;
        case 0xA8: XOR(REG_B); break; case 0xA9: XOR(REG_C); break; case 0xAA: XOR(REG_D); break; case 0xAB: XOR(REG_E); break;
        case 0xAC: XOR(REG_H); break; case 0xAD: XOR(REG_L); break; case 0xAE: XOR(rb(REG_HL)); cycles=8; break; case 0xAF: XOR(REG_A); break;
        case 0xB0: OR(REG_B); break; case 0xB1: OR(REG_C); break; case 0xB2: OR(REG_D); break; case 0xB3: OR(REG_E); break;
        case 0xB4: OR(REG_H); break; case 0xB5: OR(REG_L); break; case 0xB6: OR(rb(REG_HL)); cycles=8; break; case 0xB7: OR(REG_A); break;
        case 0xB8: CP(REG_B); break; case 0xB9: CP(REG_C); break; case 0xBA: CP(REG_D); break; case 0xBB: CP(REG_E); break;
        case 0xBC: CP(REG_H); break; case 0xBD: CP(REG_L); break; case 0xBE: CP(rb(REG_HL)); cycles=8; break; case 0xBF: CP(REG_A); break;
        case 0xC0: if(!GET_Z()){REG_PC=rb(REG_SP)|(rb(REG_SP+1)<<8); REG_SP+=2; cycles=20;}else{cycles=8;} break;
        case 0xC1: {uint8_t l=rb(REG_SP++); uint8_t h=rb(REG_SP++); SET_BC((h<<8)|l); cycles=12; break;}
        case 0xC2: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(!GET_Z()){REG_PC=a; cycles=16;}else{cycles=12;} break;}
        case 0xC3: REG_PC=rb(REG_PC)|(rb(REG_PC+1)<<8); cycles=16; break;
        case 0xC4: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(!GET_Z()){REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=a; cycles=24;}else{cycles=12;} break;}
        case 0xC5: wb(--REG_SP,REG_B); wb(--REG_SP,REG_C); cycles=16; break;
        case 0xC6: ADD(rb(REG_PC++)); cycles=8; break;
        case 0xC7: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x00; cycles=16; break;
        case 0xC8: if(GET_Z()){REG_PC=rb(REG_SP)|(rb(REG_SP+1)<<8); REG_SP+=2; cycles=20;}else{cycles=8;} break;
        case 0xC9: REG_PC=rb(REG_SP)|(rb(REG_SP+1)<<8); REG_SP+=2; cycles=16; break;
        case 0xCA: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(GET_Z()){REG_PC=a; cycles=16;}else{cycles=12;} break;}
        case 0xCB: {uint8_t c=rb(REG_PC++); uint8_t* t[]={&REG_B,&REG_C,&REG_D,&REG_E,&REG_H,&REG_L,NULL,&REG_A}; uint8_t v; int i=c&7; if(i==6){v=rb(REG_HL); cycles=16;}else{v=*t[i]; cycles=8;} uint8_t b=(c>>3)&7; uint8_t r=v; switch(c>>6){case 0: switch(b){case 0: {uint8_t carry=v>>7; r=(v<<1)|carry; SET_C(carry); break;} case 1: {uint8_t carry=v&1; r=(v>>1)|(carry<<7); SET_C(carry); break;} case 2: {uint8_t carry=v>>7; r=(v<<1)|GET_C(); SET_C(carry); break;} case 3: {uint8_t carry=v&1; r=(v>>1)|(GET_C()<<7); SET_C(carry); break;} case 4: {SET_C(v>>7); r=v<<1; break;} case 5: {SET_C(v&1); r=(v>>1)|(v&0x80); break;} case 6: {r=((v&0x0F)<<4)|((v&0xF0)>>4); SET_C(0); break;} case 7: {SET_C(v&1); r=v>>1; break;}} SET_Z(r==0); SET_N(0); SET_H(0); break; case 1: SET_Z(!((v>>b)&1)); SET_N(0); SET_H(1); break; case 2: r=v&~(1<<b); break; case 3: r=v|(1<<b); break;} if(i==6)wb(REG_HL,r); else*t[i]=r; break;}
        case 0xCC: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(GET_Z()){REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=a; cycles=24;}else{cycles=12;} break;}
        case 0xCD: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=a; cycles=24; break;}
        case 0xCE: ADC(rb(REG_PC++)); cycles=8; break;
        case 0xCF: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x08; cycles=16; break;
        case 0xD0: if(!GET_C()){REG_PC=rb(REG_SP)|(rb(REG_SP+1)<<8); REG_SP+=2; cycles=20;}else{cycles=8;} break;
        case 0xD1: {uint8_t l=rb(REG_SP++); uint8_t h=rb(REG_SP++); SET_DE((h<<8)|l); cycles=12; break;}
        case 0xD2: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(!GET_C()){REG_PC=a; cycles=16;}else{cycles=12;} break;}
        case 0xD4: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(!GET_C()){REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=a; cycles=24;}else{cycles=12;} break;}
        case 0xD5: wb(--REG_SP,REG_D); wb(--REG_SP,REG_E); cycles=16; break;
        case 0xD6: SUB(rb(REG_PC++)); cycles=8; break;
        case 0xD7: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x10; cycles=16; break;
        case 0xD8: if(GET_C()){REG_PC=rb(REG_SP)|(rb(REG_SP+1)<<8); REG_SP+=2; cycles=20;}else{cycles=8;} break;
        case 0xD9: REG_PC=rb(REG_SP)|(rb(REG_SP+1)<<8); REG_SP+=2; gb.ime=true; cycles=16; break;
        case 0xDA: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(GET_C()){REG_PC=a; cycles=16;}else{cycles=12;} break;}
        case 0xDC: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; if(GET_C()){REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=a; cycles=24;}else{cycles=12;} break;}
        case 0xDE: SBC(rb(REG_PC++)); cycles=8; break;
        case 0xDF: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x18; cycles=16; break;
        case 0xE0: wb(0xFF00+rb(REG_PC++),REG_A); cycles=12; break;
        case 0xE1: {uint8_t l=rb(REG_SP++); uint8_t h=rb(REG_SP++); SET_HL((h<<8)|l); cycles=12; break;}
        case 0xE2: wb(0xFF00+REG_C,REG_A); cycles=8; break;
        case 0xE5: wb(--REG_SP,REG_H); wb(--REG_SP,REG_L); cycles=16; break;
        case 0xE6: AND(rb(REG_PC++)); cycles=8; break;
        case 0xE7: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x20; cycles=16; break;
        case 0xE8: {int8_t o=rb(REG_PC++); uint32_t r=REG_SP+o; SET_Z(0); SET_N(0); SET_H((REG_SP&0x0F)+(o&0x0F)>0x0F); SET_C((REG_SP&0xFF)+(o&0xFF)>0xFF); REG_SP=r; cycles=16; break;}
        case 0xE9: REG_PC=REG_HL; break;
        case 0xEA: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; wb(a,REG_A); cycles=16; break;}
        case 0xEE: XOR(rb(REG_PC++)); cycles=8; break;
        case 0xEF: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x28; cycles=16; break;
        case 0xF0: REG_A=rb(0xFF00+rb(REG_PC++)); cycles=12; break;
        case 0xF1: {uint8_t l=rb(REG_SP++); uint8_t h=rb(REG_SP++); REG_F=l&0xF0; REG_A=h; cycles=12; break;}
        case 0xF2: REG_A=rb(0xFF00+REG_C); cycles=8; break;
        case 0xF3: gb.ime=false; break; case 0xF5: wb(--REG_SP,REG_A); wb(--REG_SP,REG_F); cycles=16; break;
        case 0xF6: OR(rb(REG_PC++)); cycles=8; break;
        case 0xF7: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x30; cycles=16; break;
        case 0xF8: {int8_t o=rb(REG_PC++); uint32_t r=REG_SP+o; SET_Z(0); SET_N(0); SET_H((REG_SP&0x0F)+(o&0x0F)>0x0F); SET_C((REG_SP&0xFF)+(o&0xFF)>0xFF); SET_HL(r); cycles=12; break;}
        case 0xF9: REG_SP=REG_HL; cycles=8; break;
        case 0xFA: {uint16_t a=rb(REG_PC)|(rb(REG_PC+1)<<8); REG_PC+=2; REG_A=rb(a); cycles=16; break;}
        case 0xFB: gb.ime=true; break; case 0xFE: CP(rb(REG_PC++)); cycles=8; break;
        case 0xFF: REG_SP-=2; wb(REG_SP,REG_PC&0xFF); wb(REG_SP+1,REG_PC>>8); REG_PC=0x38; cycles=16; break;
        default: fprintf(stderr,"Unimplemented opcode: 0x%02X at 0x%04X\n",opcode,REG_PC-1); is_running=false; break;
    } return cycles;
}

// --- Main Application ---
void gb_init() {
    memset(&gb, 0, sizeof(GameBoy));
    gb.reg.pc = 0x0000; // Start execution from the boot ROM
    gb.reg.sp = 0xFFFE;
    gb.boot_rom_active = true; // Boot ROM is initially active
    gb.joypad_state = 0xFF;

    // Default I/O registers - the boot ROM will set these properly
    wb(0xFF40, 0x91);
    wb(0xFF41, 0x85);
}

void gb_load_boot_rom(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("Boot ROM '%s' not found. Skipping.\n", path);
        gb.boot_rom_active = false;
        gb.reg.pc = 0x0100; // Set PC to game entry point
        // Set post-boot ROM values
        gb.reg.a = 0x01; gb.reg.f = 0xB0;
        SET_BC(0x0013); SET_DE(0x00D8); SET_HL(0x014D);
    } else {
        fread(gb.mem.boot_rom, 1, 256, f);
        fclose(f);
    }
}

void gb_load_rom(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("Error loading ROM"); exit(1); }
    fseek(f, 0, SEEK_END); gb.mbc.rom_size = ftell(f); rewind(f);
    gb.mem.rom = (uint8_t*)malloc(gb.mbc.rom_size);
    fread(gb.mem.rom, 1, gb.mbc.rom_size, f); fclose(f);

    // Set save file path
    strncpy(gb.save_path, path, sizeof(gb.save_path) - 5);
    strcat(gb.save_path, ".sav");
    
    // Parse Cartridge Header
    uint8_t mbc_type_code = gb.mem.rom[0x0147];
    switch (mbc_type_code) {
        case 0x00: gb.mbc.type = 0; break;
        case 0x01: case 0x02: case 0x03: gb.mbc.type = 1; break;
        case 0x05: case 0x06: gb.mbc.type = 2; break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: gb.mbc.type = 3; break;
        case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: gb.mbc.type = 5; break;
        default: printf("Warning: Unsupported MBC type 0x%02X\n", mbc_type_code); gb.mbc.type = 0; break;
    }
    gb.mbc.has_battery = (mbc_type_code == 0x03 || mbc_type_code == 0x06 || mbc_type_code == 0x0F || mbc_type_code == 0x10 || mbc_type_code == 0x13 || mbc_type_code == 0x1B || mbc_type_code == 0x1E);

    uint8_t ram_size_code = gb.mem.rom[0x0149];
    if (gb.mbc.type == 2) gb.mbc.ram_size = 512;
    else switch (ram_size_code) {
        case 0x02: gb.mbc.ram_size = 8 * 1024; break;
        case 0x03: gb.mbc.ram_size = 32 * 1024; break;
        case 0x04: gb.mbc.ram_size = 128 * 1024; break;
        case 0x05: gb.mbc.ram_size = 64 * 1024; break;
        default: gb.mbc.ram_size = 0; break;
    }
    
    if (gb.mbc.ram_size > 0) {
        gb.mem.eram = (uint8_t*)calloc(gb.mbc.ram_size, 1);
    }
    
    gb.mbc.rom_bank = 1;
}

void gb_load_sram() {
    if (!gb.mbc.has_battery || !gb.mem.eram) return;
    FILE* f = fopen(gb.save_path, "rb");
    if (f) {
        fread(gb.mem.eram, 1, gb.mbc.ram_size, f);
        if (gb.mbc.type == 3) { // Load RTC data
            fread(&gb.mbc.rtc, sizeof(gb.mbc.rtc), 1, f);
        }
        fclose(f);
        printf("Loaded save file: %s\n", gb.save_path);
    }
}

void gb_save_sram() {
    if (!gb.mbc.has_battery || !gb.mem.eram) return;
    FILE* f = fopen(gb.save_path, "wb");
    if (f) {
        fwrite(gb.mem.eram, 1, gb.mbc.ram_size, f);
         if (gb.mbc.type == 3) { // Save RTC data
            rtc_update(); // Ensure clock is current before saving
            fwrite(&gb.mbc.rtc, sizeof(gb.mbc.rtc), 1, f);
        }
        fclose(f);
        printf("Saved game to: %s\n", gb.save_path);
    }
}


void gb_run_frame() {
    int cycles_this_frame = 0;
    while (cycles_this_frame < CYCLES_PER_FRAME) {
        int cycles = gb.halted ? 4 : execute_opcode();
        cycles_this_frame += cycles;
        ppu_step(cycles);
        timer_step(cycles);
        handle_interrupts();
    }
}

void handle_input(SDL_Event* event) {
    static const SDL_Scancode sc_map[8] = {
        SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN
    };

    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) return;
    // ignore repeat events so holding a key doesn't spam interrupts
    if (event->type == SDL_KEYDOWN && event->key.repeat) return;

    SDL_Scancode sc = event->key.keysym.scancode;
    for (int i = 0; i < 8; ++i) {
        if (sc == sc_map[i]) {
            if (event->type == SDL_KEYDOWN) {
                gb.joypad_state &= ~(1 << i);   // pressed => pull bit LOW
                request_interrupt(4);           // only fire on press
            } else {
                gb.joypad_state |= (1 << i);    // released => set bit HIGH
            }
            break;
        }
    }
}


int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <rom_file.gb>\n", argv[0]); return 1; }
    
    gb_init();
    gb_load_boot_rom("dmg_boot.bin");
    gb_load_rom(argv[1]);
    gb_load_sram();

    SDL_Init(SDL_INIT_VIDEO);
    window = SDL_CreateWindow("Cortez Game Boy Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH * 3, SCREEN_HEIGHT * 3, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);

    while (is_running) {
        Uint32 frame_start = SDL_GetTicks();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) is_running = false;
            handle_input(&event);
        }
        
        gb_run_frame();

        SDL_UpdateTexture(texture, NULL, gb.ppu.framebuffer, SCREEN_WIDTH * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        int frame_time = SDL_GetTicks() - frame_start;
        if ((1000 / 60) > frame_time) SDL_Delay((1000 / 60) - frame_time);
    }
    
    gb_save_sram();
    free(gb.mem.rom);
    if(gb.mem.eram) free(gb.mem.eram);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}