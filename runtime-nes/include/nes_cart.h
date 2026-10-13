/*
 * NES cartridges: the iNES / NES 2.0 header and the mapper boards.
 *
 * A cartridge maps PRG ROM into $8000-$FFFF in 8 KiB pages, optional PRG
 * RAM into $6000-$7FFF, CHR ROM or RAM into the PPU's $0000-$1FFF in 1 KiB
 * pages, and decides how the PPU's four nametable slots share the
 * console's 2 KiB of nametable RAM. All of that is kept here as offsets,
 * never host pointers, so the struct can be saved and compared as is.
 *
 * The PRG page table is also the recompiler's banking model: recompiled
 * code is keyed by the 8 KiB ROM page mapped at its address (nes_recomp.h),
 * and prg_gen counts every change of the table so native code can tell
 * that the bytes around it were switched out.
 */
#ifndef NES_CART_H
#define NES_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NesMachine;

#define NES_PRG_PAGE 0x2000u       /* PRG pages are 8 KiB */
#define NES_CHR_PAGE 0x0400u       /* CHR pages are 1 KiB */
#define NES_PRG_ROM_MAX 0x400000u  /* 4 MiB */
#define NES_CHR_ROM_MAX 0x200000u  /* 2 MiB */
#define NES_PRG_RAM_MAX 0x8000u    /* 32 KiB (MMC1 SXROM boards) */
#define NES_CHR_RAM_MAX 0x8000u    /* 32 KiB */
#define NES_NT_RAM_SIZE 0x1000u    /* 2 KiB in the console, 4 KiB on four-screen boards */

typedef enum {
    NES_MIRROR_HORIZONTAL, /* $2000=$2400, $2800=$2C00 */
    NES_MIRROR_VERTICAL,   /* $2000=$2800, $2400=$2C00 */
    NES_MIRROR_SINGLE_0,   /* every slot is the first 1 KiB */
    NES_MIRROR_SINGLE_1,   /* every slot is the second 1 KiB */
    NES_MIRROR_FOUR,       /* four separate screens (cartridge RAM) */
} NesMirroring;

typedef enum {
    NES_REGION_NTSC,
    NES_REGION_PAL,
    NES_REGION_DUAL,
    NES_REGION_DENDY,
} NesRegion;

/* What the file header says. */
typedef struct NesCartInfo {
    uint16_t mapper;       /* iNES mapper number */
    uint8_t submapper;     /* NES 2.0 submapper, else 0 */
    bool nes20;            /* NES 2.0 header */
    bool battery;          /* PRG RAM (or other memory) is battery backed */
    bool trainer;          /* 512 bytes at $7000 precede PRG ROM in the file */
    uint8_t mirroring;     /* NesMirroring soldered on the board (the mapper may override) */
    uint8_t region;        /* NesRegion */
    uint32_t prg_size;     /* PRG ROM bytes */
    uint32_t chr_size;     /* CHR ROM bytes; 0 = the board has CHR RAM */
    uint32_t prg_ram_size; /* PRG RAM bytes mapped at $6000-$7FFF (0 = none) */
    uint32_t chr_ram_size; /* CHR RAM bytes (0 with CHR ROM) */
    uint32_t prg_offset;   /* where PRG ROM starts in the file */
    uint32_t chr_offset;   /* where CHR ROM starts in the file */
    uint32_t prg_crc32;    /* CRC-32 of PRG ROM, for the game database quirks */
    /* The board's name when the mapper is implemented, else NULL: the ROM
     * is recognised but cannot run. */
    const char *board;
} NesCartInfo;

/* The mapper's state. reg[] and the named fields are used differently by
 * each board; see nes_cart.c. */
typedef struct NesCart {
    /* Mapping, rebuilt by the board after every register write. */
    uint32_t prg_page[4];    /* PRG ROM offset of the 8 KiB page at $8000, $A000, $C000, $E000 */
    uint32_t chr_page[8];    /* CHR offset of the 1 KiB page at $0000 ... $1C00 */
    uint32_t nt_page[4];     /* nametable RAM offset of $2000, $2400, $2800, $2C00 */
    uint32_t prg_ram_page;   /* PRG RAM offset of the 8 KiB at $6000 */
    uint8_t prg_ram_enable;  /* PRG RAM answers reads and writes */
    uint8_t prg_ram_protect; /* PRG RAM ignores writes */
    uint8_t chr_writable;    /* CHR is RAM */
    uint8_t irq;             /* the cartridge's IRQ line (level) */
    uint64_t prg_gen;        /* bumped whenever prg_page[] changes */

    /* Registers. */
    uint8_t reg[16];
    uint8_t shift;           /* MMC1: serial shift register */
    uint8_t shift_count;     /* MMC1: bits shifted in */
    uint64_t last_write;     /* MMC1: CPU cycle of the last register write */
    uint8_t bank_select;     /* MMC3: $8000 */
    uint8_t irq_latch;       /* MMC3: $C000 */
    uint8_t irq_counter;
    uint8_t irq_reload;      /* MMC3: $C001 written, reload at the next clock */
    uint8_t irq_enable;
    uint8_t a12;             /* MMC3/MMC2: the PPU A12 line as last seen */
    uint64_t a12_low_since;  /* MMC3: PPU dot count when A12 last fell */
    uint8_t latch[2];        /* MMC2/MMC4: the two CHR latches */
} NesCart;

/* Read a file's header. Returns false when it is not an iNES image or its
 * sizes do not fit the file. info->board is NULL for an unimplemented
 * mapper. */
bool nes_cart_parse(const uint8_t *file, size_t size, NesCartInfo *info);

/* Power on: registers and mapping in their power-on state. */
void nes_cart_power(struct NesMachine *m);

/* CPU side, $4020-$FFFF. `mdr` is the open-bus value. */
uint8_t nes_cart_read(struct NesMachine *m, uint16_t addr, uint8_t mdr);
void nes_cart_write(struct NesMachine *m, uint16_t addr, uint8_t value);

/* PPU side, $0000-$3EFF (pattern tables and nametables), and the address
 * lines changing without an access. `dots` is the PPU's dot count, for the
 * boards that time the A12 line. */
uint8_t nes_cart_ppu_read(struct NesMachine *m, uint16_t addr, uint64_t dots);
void nes_cart_ppu_write(struct NesMachine *m, uint16_t addr, uint8_t value, uint64_t dots);
void nes_cart_ppu_address(struct NesMachine *m, uint16_t addr, uint64_t dots);

/* True when the board raises its IRQ from PPU fetches, so the machine must
 * keep the PPU caught up closely while that interrupt is enabled. */
bool nes_cart_watches_ppu(const struct NesMachine *m);

/* The 8 KiB PRG ROM page number (ROM offset / 8 KiB) mapped at a CPU
 * address in $8000-$FFFF. */
uint32_t nes_cart_prg_page_at(const struct NesMachine *m, uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* NES_CART_H */
