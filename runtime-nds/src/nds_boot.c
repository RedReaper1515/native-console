/*
 * Direct boot: put the machine in the state the DS's firmware leaves when
 * it starts the game in the card slot, without running any firmware
 * (GBATEK, "BIOS RAM Usage", the part on NDS memory and registers at
 * cartridge boot time).
 *
 * The firmware reads the header, copies the two binaries to the RAM
 * addresses the header names, leaves the header and a few facts about the
 * card and the console in the top of main RAM, sets each CPU's stack
 * pointers, and jumps to both entry points.
 */
#include "nds.h"

#include <string.h>

/* A CPU-visible address in main RAM. 0x027FFxxx is the mirror the firmware
 * and the SDK both use for the system area at the top. */
static uint8_t *ram(NdsMachine *m, uint32_t addr)
{
    return m->main_ram + (addr & (NDS_MAIN_RAM_SIZE - 1u));
}

/* Copy a binary from the ROM to where the header says it loads, through
 * that CPU's view of memory with the mappings in force at boot. Only RAM
 * that exists then can be a destination: main RAM and, for the ARM7, its
 * own WRAM and the shared WRAM (which the firmware has handed to it). */
static void load_binary(NdsMachine *m, int cpu, uint32_t rom_offset, uint32_t load_addr, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t addr = load_addr + i;
        uint8_t byte = m->rom[rom_offset + i];
        if ((addr >> 24) == 0x02u) {
            *ram(m, addr) = byte;
        } else if (cpu == NDS_ARM7 && (addr >> 24) == 0x03u) {
            if (addr & 0x00800000u) m->arm7_wram[addr & (NDS_ARM7_WRAM_SIZE - 1u)] = byte;
            else m->shared_wram[addr & (NDS_SHARED_WRAM_SIZE - 1u)] = byte;
        }
    }
}

void nds_boot_direct(NdsMachine *m)
{
    const uint8_t *header = m->rom;
    uint32_t arm9_offset = nds_get_le32(header + 0x20), arm9_entry = nds_get_le32(header + 0x24);
    uint32_t arm9_load = nds_get_le32(header + 0x28), arm9_size = nds_get_le32(header + 0x2C);
    uint32_t arm7_offset = nds_get_le32(header + 0x30), arm7_entry = nds_get_le32(header + 0x34);
    uint32_t arm7_load = nds_get_le32(header + 0x38), arm7_size = nds_get_le32(header + 0x3C);

    /* Memory control as the firmware leaves it: the whole shared WRAM with
     * the ARM7; the card and GBA slots assigned to the ARM7, synchronous
     * main memory, ARM7 priority. */
    m->wramcnt = 3;
    m->exmemcnt[NDS_ARM9] = 0xE880;
    m->exmemcnt[NDS_ARM7] = 0;
    nds_wram_remap(m);

    load_binary(m, NDS_ARM9, arm9_offset, arm9_load, arm9_size);
    load_binary(m, NDS_ARM7, arm7_offset, arm7_load, arm7_size);
    /* The first 2 KiB of an ARM9 program at ROM 0x4000 is the secure area.
     * The BIOS checks the ID it starts with once decrypted, "encryObj", and
     * overwrites it; a dump that still shows the ID gets the same. */
    if (arm9_offset == 0x4000u && arm9_size >= 8 && (arm9_load >> 24) == 0x02u &&
        memcmp(m->rom + arm9_offset, "encryObj", 8) == 0) {
        nds_put_le32(ram(m, arm9_load), 0xE7FFDEFFu);
        nds_put_le32(ram(m, arm9_load + 4u), 0xE7FFDEFFu);
    }

    /* The system area at the top of main RAM. */
    memcpy(ram(m, 0x027FFE00u), header, 0x170);
    nds_put_le32(ram(m, 0x027FF800u), m->chip_id);
    nds_put_le32(ram(m, 0x027FF804u), m->chip_id);
    nds_put_le16(ram(m, 0x027FF808u), nds_get_le16(header + 0x15E)); /* header CRC */
    nds_put_le16(ram(m, 0x027FF80Au), nds_get_le16(header + 0x06C)); /* secure area CRC */
    nds_put_le16(ram(m, 0x027FF850u), 0x5835);                       /* ARM7 BIOS CRC */
    nds_put_le32(ram(m, 0x027FF880u), 7);                            /* message from ARM9 to ARM7 */
    nds_put_le32(ram(m, 0x027FF884u), 6);                            /* ARM7 boot task */
    nds_put_le32(ram(m, 0x027FFC00u), m->chip_id);
    nds_put_le32(ram(m, 0x027FFC04u), m->chip_id);
    nds_put_le16(ram(m, 0x027FFC08u), nds_get_le16(header + 0x15E));
    nds_put_le16(ram(m, 0x027FFC0Au), nds_get_le16(header + 0x06C));
    nds_put_le16(ram(m, 0x027FFC10u), 0x5835);
    nds_put_le16(ram(m, 0x027FFC30u), 0xFFFF);                       /* no GBA cartridge */
    nds_put_le16(ram(m, 0x027FFC40u), 1);                            /* boot indicator: normal */
    /* The owner's settings, as the firmware copies them from the copy in
     * force in its flash, and where in the flash they are. */
    uint32_t settings_at;
    if (nds_firmware_current_settings(m->firmware, &settings_at)) {
        memcpy(ram(m, NDS_USER_SETTINGS_RAM_ADDRESS), m->firmware + settings_at, NDS_USER_SETTINGS_SIZE);
    } else {
        nds_user_settings_ram_block(&m->settings, ram(m, NDS_USER_SETTINGS_RAM_ADDRESS));
    }
    nds_put_le32(ram(m, 0x027FF868u), (uint32_t)nds_get_le16(m->firmware + 0x20) * 8u);

    /* ARM9: system mode, the stacks GBATEK lists, DTCM over 0x03000000 so
     * those stacks and the interrupt handler's cells at DTCM+0x3FF8 exist,
     * ITCM mirrored through the first 32 MiB. */
    NdsCpu *arm9 = &m->arm9;
    nds9_cp15_write(arm9, 0x910, 0x0300000Au);
    nds9_cp15_write(arm9, 0x911, 0x00000020u);
    nds9_cp15_write(arm9, 0x100, arm9->cp15.control | NDS_CP15_DTCM_ENABLE | NDS_CP15_ITCM_ENABLE);
    nds_cpu_set_cpsr(arm9, NDS_MODE_SVC | NDS_CPSR_I | NDS_CPSR_F);
    arm9->r[13] = 0x03003FC0u;
    nds_cpu_set_cpsr(arm9, NDS_MODE_IRQ | NDS_CPSR_I | NDS_CPSR_F);
    arm9->r[13] = 0x03003F80u;
    nds_cpu_set_cpsr(arm9, NDS_MODE_SYS);
    memset(arm9->r, 0, 12 * sizeof arm9->r[0]);
    arm9->r[13] = 0x03002F7Cu;
    arm9->r[12] = arm9->r[14] = arm9_entry;
    arm9->r[15] = (arm9_entry & ~3u) + 8;

    NdsCpu *arm7 = &m->arm7;
    nds_cpu_set_cpsr(arm7, NDS_MODE_SVC | NDS_CPSR_I | NDS_CPSR_F);
    arm7->r[13] = 0x0380FFC0u;
    nds_cpu_set_cpsr(arm7, NDS_MODE_IRQ | NDS_CPSR_I | NDS_CPSR_F);
    arm7->r[13] = 0x0380FF80u;
    nds_cpu_set_cpsr(arm7, NDS_MODE_SYS);
    memset(arm7->r, 0, 12 * sizeof arm7->r[0]);
    arm7->r[13] = 0x0380FD80u;
    arm7->r[12] = arm7->r[14] = arm7_entry;
    arm7->r[15] = (arm7_entry & ~3u) + 8;

    /* I/O the firmware has set. */
    m->postflg[NDS_ARM9] = m->postflg[NDS_ARM7] = 1;
    m->powcnt1 = 0x820F; /* both screens and all engines on, engine A on top */
    m->powcnt2 = 0x0001; /* speakers on */
    m->biosprot = 0x1204;
    nds_sound_write7(m, 0x04000504u, 0x0200, 2); /* SOUNDBIAS: the output's mid level */
    /* The card is in its main data mode, released from reset. */
    m->card.romctrl[NDS_ARM9] = m->card.romctrl[NDS_ARM7] = 0x20000000u;
}
