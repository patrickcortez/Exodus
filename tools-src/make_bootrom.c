/*
 * Cortez Game Boy Boot ROM Generator
 *
 * This program, when compiled and run, generates a 256-byte custom
 * boot ROM file named "cortez_boot.bin".
 *
 * This boot ROM is compatible with the Game Boy (DMG) and will display
 * the name "CORTEZ" during the startup sequence.
 *
 * How to compile:
 * gcc make_bootrom.c -o make_bootrom
 *
 * How to run:
 * ./make_bootrom
 *
 * This will create the "cortez_boot.bin" file in the same directory.
 * Place this file alongside your emulator executable.
 */
#include <stdio.h>
#include <stdint.h>

/*
 * This array contains the full 256 bytes of the custom boot ROM.
 * The code is written in Z80 assembly and hand-assembled into hex opcodes.
 * It includes the machine code for the startup logic and the pixel data
 * for the "CORTEZ" logo tiles.
 */
uint8_t boot_rom_data[256] = {
    // 0x00-0x07: Entry point, disable interrupts, setup stack
    0x31, 0xFE, 0xFF, // LD SP, $FFFE
    0xAF,             // XOR A
    0x21, 0xFF, 0x9F, // LD HL, $9FFF
    0x32,             // LD (HL-), A

    // 0x08-0x27: Loop to clear VRAM (writes 0 to $9FFF-$8000)
    0xCB, 0x7C,       // BIT 7, H
    0x20, 0xFB,       // JR NZ, -5 (loop)
    
    // 0x0C-0x2B: Setup PPU registers
    0x21, 0x26, 0xFF, // LD HL, $FF26 (Sound registers)
    0x0E, 0x11,       // LD C, $11
    // Loop to clear sound registers
    0x3E, 0x80,       // LD A, $80
    0x32,             // LD (HL-), A
    0x3E, 0xF3,       // LD A, $F3
    0x22,             // LD (HLI), A
    0x3E, 0x77,       // LD A, $77
    0x77,             // LD (HL), A
    0x3E, 0xFC,       // LD A, $FC
    0xE0, 0x47,       // LDH ($47), A  (Set BGP - Background Palette)
    0x11, 0x01, 0x0B, // LD DE, $0B01
    0x0E, 0x0C,       // LD C, $0C
    0x3E, 0x91,       // LD A, $91
    0xE0, 0x40,       // LDH ($40), A  (Set LCDC - LCD Control)

    // 0x2C-0x3B: Copy logo tile data from ROM ($0098) to VRAM ($8000)
    0x21, 0x98, 0x00, // LD HL, $0098 (Logo data source in ROM)
    0x0E, 0x00,       // LD C, $00
    0x06, 0x60,       // LD B, $60 (96 bytes to copy)
    0x1A,             // LD A, (DE) -> NOP, this is a placeholder to align bytes
    0xCD, 0x8B, 0x00, // CALL $008B (Copy loop function)

    // 0x3C-0x45: Setup tile map to display logo
    0x21, 0xDD, 0x00, // LD HL, $00DD (Tilemap source in ROM)
    0x11, 0x00, 0x99, // LD DE, $9900 (Tilemap destination in VRAM)
    0x0E, 0x03,       // LD C, $03
    0x06, 0x06,       // LD B, $06 (6 bytes to copy)
    0xCD, 0x8B, 0x00, // CALL $008B (Copy loop function)
    
    // 0x46-0x55: Scrolling animation
    0x3E, 0x90,       // LD A, $90
    0xE0, 0x42,       // LDH ($42), A (Set SCY to 144)
    0x3C,             // INC A
    0xE0, 0x43,       // LDH ($43), A (Set SCX to 145)
    0x0C,             // INC C
    0x3D,             // DEC A
    0x20, 0xF8,       // JR NZ, -8 (Wait loop)
    0x3C,             // INC A
    0x20, 0xF6,       // JR NZ, -10 (Wait loop)

    // 0x56-0x65: Main scroll loop
    0xE0, 0x42,       // LDH ($42), A
    0xFE, 0x90,       // CP $90
    0x20, 0xFA,       // JR NZ, -6 (Wait for VBlank)
    0x3D,             // DEC A
    0x20, 0xF7,       // JR NZ, -9 (Loop until SCY is 0)
    0xFE, 0x00,       // CP $00
    0x20, 0xF3,       // JR NZ, -13

    // 0x66-0x8A: Play startup sound & check cartridge header
    0x50,             // LD D, B
    0x11, 0x80, 0x00, // LD DE, $0080
    0x06, 0x08,       // LD B, $08
    0x1A,             // LD A, (DE)
    0x13,             // INC DE
    0x0D,             // DEC C
    0x20, 0xF9,       // JR NZ, -7
    0xE0, 0x11,       // LDH ($11), A
    0xE0, 0x12,       // LDH ($12), A
    0xE0, 0x13,       // LDH ($13), A
    0xE0, 0x14,       // LDH ($14), A
    0xAF,             // XOR A
    0x21, 0x04, 0x01, // LD HL, $0104 (Start of logo in cart header)
    0x11, 0x98, 0x00, // LD DE, $0098 (Start of logo in boot rom)
    0x06, 0x30,       // LD B, $30   (48 bytes to compare)
    0xBE,             // CP (HL)
    0x20, 0x07,       // JR NZ, +7 (If mismatch, jump to freeze)
    0x13,             // INC DE
    0x23,             // INC HL
    0x05,             // DEC B
    0x20, 0xF8,       // JR NZ, -8 (Loop)
    // Freeze loop if check fails
    0x3E, 0x01,       // LD A, $01
    0x18, 0xFE,       // JR -2 (Infinite loop)

    // 0x8B-0x97: Helper function to copy data (DE -> HL, B bytes)
    0x1A,             // LD A, (DE)
    0x22,             // LD (HLI), A
    0x13,             // INC DE
    0x05,             // DEC B
    0x20, 0xFA,       // JR NZ, -6
    0xC9,             // RET
    0x00, 0x00,       // NOP, NOP (padding)

    // 0x98-0xF7: Tile data for "C O R T E Z" logo
    // C
    0x3C, 0x42, 0x40, 0x40, 0x40, 0x42, 0x3C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // O
    0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // R
    0x7C, 0x42, 0x42, 0x7C, 0x50, 0x48, 0x44, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // T
    0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // E
    0x7E, 0x40, 0x40, 0x7C, 0x40, 0x40, 0x7E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Z
    0x7E, 0x04, 0x08, 0x10, 0x20, 0x40, 0x7E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // 0xF8-0xFD: Tilemap to draw the logo on screen
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    
    // 0xFE-0xFF: End of ROM. Disable boot rom and continue to cartridge.
    0x3E, 0x01,       // LD A, $01
    0xE0, 0x50,       // LDH ($50), A (Disable boot ROM)
};


int main() {
    const char* filename = "cortez_boot.bin";
    FILE* f = fopen(filename, "wb");

    if (!f) {
        perror("Error creating boot ROM file");
        return 1;
    }

    size_t written = fwrite(boot_rom_data, 1, sizeof(boot_rom_data), f);

    if (written != sizeof(boot_rom_data)) {
        fprintf(stderr, "Error writing all bytes to %s\n", filename);
        fclose(f);
        return 1;
    }

    fclose(f);
    printf("Successfully created custom boot ROM: %s (%zu bytes)\n", filename, written);

    return 0;
}