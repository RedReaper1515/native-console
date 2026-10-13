#include "gbrt.h"

#include <stdio.h>
#include <string.h>

/* CGB post-boot state: the header-dependent DIV at the $0100 hand-off and the
 * I/O readback that differs between CGB mode and DMG compatibility mode. */

typedef struct {
    const char* name;
    const char* title;
    uint8_t cgb_flag;
    uint8_t old_licensee;
    const char* new_licensee;
    uint16_t div_counter;
} BootDivCase;

/* The first case is Mooneye misc/boot_div-cgbABCDE's header path, verified on
 * CGB A-E hardware. The rest follow the boot ROM's header-dependent paths. */
static const BootDivCase boot_div_cases[] = {
    {"DMG cart, non-Nintendo licensee", "ANY TITLE", 0x00, 0x33, "ZZ", 0x2678},
    {"DMG cart, old licensee $01, checksum in table", "TETRIS", 0x00, 0x01, "ZZ", 0x2CCC},
    {"DMG cart, new licensee \"01\", checksum in table", "ZELDA", 0x00, 0x33, "01", 0x316C},
    {"CGB cart, non-Nintendo licensee", "ANY TITLE", 0x80, 0x33, "ZZ", 0x1E9C},
    {"CGB cart, Nintendo licensee, checksum not in table", "TETRIS", 0xC0, 0x01, "ZZ", 0x2FB0},
};

static uint8_t rom[32u * 1024u];

static GBContext* make_cgb_context(const char* title,
                                   uint8_t cgb_flag,
                                   uint8_t old_licensee,
                                   const char* new_licensee) {
    GBConfig config;
    memset(&config, 0, sizeof(config));
    config.model = GB_MODEL_CGB;
    config.cgb_compatibility_mode = (cgb_flag & 0x80) == 0;

    memset(rom, 0, sizeof(rom));
    memcpy(&rom[0x134], title, strlen(title));
    rom[0x143] = cgb_flag;
    rom[0x144] = (uint8_t)new_licensee[0];
    rom[0x145] = (uint8_t)new_licensee[1];
    rom[0x14B] = old_licensee;

    GBContext* ctx = gb_context_create(&config);
    if (!ctx) {
        return NULL;
    }
    if (!gb_context_load_rom(ctx, rom, sizeof(rom))) {
        gb_context_destroy(ctx);
        return NULL;
    }
    gb_context_reset(ctx, true);
    return ctx;
}

static int test_post_boot_div(void) {
    int failures = 0;
    for (size_t i = 0; i < sizeof(boot_div_cases) / sizeof(boot_div_cases[0]); i++) {
        const BootDivCase* c = &boot_div_cases[i];
        GBContext* ctx = make_cgb_context(c->title, c->cgb_flag,
                                          c->old_licensee, c->new_licensee);
        if (!ctx) {
            return 2;
        }
        if (ctx->div_counter != c->div_counter ||
            gb_read8(ctx, 0xFF04) != (uint8_t)(c->div_counter >> 8)) {
            fprintf(stderr, "%s: div_counter=%04X, expected %04X\n",
                    c->name, ctx->div_counter, c->div_counter);
            failures++;
        }
        gb_context_destroy(ctx);
    }
    return failures ? 1 : 0;
}

static int expect_read(GBContext* ctx, const char* mode, uint16_t addr,
                       uint8_t mask, uint8_t expected) {
    const uint8_t value = gb_read8(ctx, addr);
    if ((value & mask) != expected) {
        fprintf(stderr, "%s: $%04X reads %02X (mask %02X), expected %02X\n",
                mode, addr, value, mask, expected);
        return 1;
    }
    return 0;
}

/* Registers without hardware behind them on CGB, in either mode. */
static const uint16_t cgb_unmapped_io[] = {
    0xFF4C, 0xFF4E, 0xFF50, 0xFF57, 0xFF60, 0xFF67,
    0xFF6D, 0xFF6F, 0xFF71, 0xFF78, 0xFF7F,
};

static int test_unmapped_io(GBContext* ctx, const char* mode) {
    int failures = 0;
    for (size_t i = 0; i < sizeof(cgb_unmapped_io) / sizeof(cgb_unmapped_io[0]); i++) {
        gb_write8(ctx, cgb_unmapped_io[i], 0x00);
        failures += expect_read(ctx, mode, cgb_unmapped_io[i], 0xFF, 0xFF);
    }
    return failures;
}

static int test_compatibility_mode_io(void) {
    const char* mode = "compatibility mode";
    GBContext* ctx = make_cgb_context("ANY TITLE", 0x00, 0x33, "ZZ");
    if (!ctx) {
        return 2;
    }
    int failures = 0;

    /* Mooneye misc/bits/unused_hwio-C: the SC clock-speed bit is CGB-mode
     * only, and the palette registers the boot ROM filled are locked. */
    gb_write8(ctx, 0xFF02, 0x00);
    failures += expect_read(ctx, mode, 0xFF02, 0x7E, 0x7E);
    for (uint16_t addr = 0xFF68; addr <= 0xFF6B; addr++) {
        gb_write8(ctx, addr, 0x00);
        failures += expect_read(ctx, mode, addr, 0xFF, 0xFF);
    }
    failures += test_unmapped_io(ctx, mode);

    gb_context_destroy(ctx);
    return failures ? 1 : 0;
}

static int test_cgb_mode_io(void) {
    const char* mode = "CGB mode";
    GBContext* ctx = make_cgb_context("ANY TITLE", 0x80, 0x33, "ZZ");
    if (!ctx) {
        return 2;
    }
    int failures = 0;

    gb_write8(ctx, 0xFF02, 0x00);
    failures += expect_read(ctx, mode, 0xFF02, 0x7E, 0x7C);
    gb_write8(ctx, 0xFF68, 0x05);
    failures += expect_read(ctx, mode, 0xFF68, 0xFF, 0x45);
    gb_write8(ctx, 0xFF6A, 0x86);
    failures += expect_read(ctx, mode, 0xFF6A, 0xFF, 0xC6);
    failures += test_unmapped_io(ctx, mode);

    gb_context_destroy(ctx);
    return failures ? 1 : 0;
}

int main(void) {
    int result = test_post_boot_div();
    if (result != 0) {
        return result;
    }
    result = test_compatibility_mode_io();
    if (result != 0) {
        return result;
    }
    result = test_cgb_mode_io();
    if (result != 0) {
        return result;
    }
    puts("CGB post-boot DIV and I/O readback match hardware");
    return 0;
}
