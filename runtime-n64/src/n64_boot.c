/*
 * The boot. On the console, the PIF ROM's IPL1 and IPL2 (Nintendo
 * firmware, which this project never uses) load the cartridge's own boot
 * code, IPL3, into the RSP's DMEM and run it; IPL3 initialises RDRAM,
 * copies the first megabyte of the game after the header to the entry
 * point and jumps there. Some CICs' IPL3 (6105) also leave state that the
 * game later checks: values in RDRAM, an RSP task's leftovers, the answer
 * to the CIC's challenge.
 *
 * When the cartridge's IPL3 is a known one (n64_cic_from_ipl3), only IPL1
 * and IPL2 are done here, as documented on n64brew (PIF-NUS, IPL3): the
 * header and IPL3 copied to DMEM, PI domain 1 timing from the first header
 * word, s3 the ROM type (0, cartridge), s4 the TV type (1 NTSC, 0 PAL),
 * s5 the reset type (0, cold), s6 the CIC's IPL3 seed, s7 the version,
 * sp at the end of IMEM, t3 at IPL3's start; then IPL3 itself runs from
 * the user's ROM, against the PIF's boot handshake (n64_pif.c) and the
 * RDRAM chips (n64_rdram.c).
 *
 * Otherwise (homebrew with its own boot code, or --hle-boot), the machine
 * is put straight into the state IPL3 leaves:
 *
 *  - 1 MiB of ROM from offset 0x1000 at the entry point (CIC 6103 and 6106
 *    games give an entry point 1 and 2 MiB higher than where they load);
 *  - IPL3 itself at DMEM 0x40, as left there;
 *  - the registers above;
 *  - the RDRAM size at 0x80000318 (0x800003F0 for CIC 6105);
 *  - the RI and the RDRAM chips configured;
 *  - Status with COP0 and COP1 usable and FR set, Cause.CE 3; the RSP
 *    halted.
 */
#include "n64_io.h"

#include <string.h>

static void ipl2_registers(N64Machine *m)
{
    N64Cpu *c = &m->cpu;
    const N64Cart *cart = &m->cart;
    const uint8_t *h = m->rom;
    m->pi_bsd[0] = h[3];         /* DOM1 latency */
    m->pi_bsd[1] = h[2];         /* DOM1 pulse width */
    m->pi_bsd[2] = h[1] & 0x0Fu; /* DOM1 page size */
    m->pi_bsd[3] = h[1] >> 4;    /* DOM1 release */
    c->gpr[19] = 0;                                   /* s3: ROM type */
    c->gpr[20] = cart->pal ? 0u : 1u;                 /* s4: TV type */
    c->gpr[21] = 0;                                   /* s5: cold reset */
    c->gpr[22] = cart->cic_seed;                      /* s6: IPL3 seed */
    c->gpr[23] = 0;                                   /* s7: version */
    c->gpr[29] = 0xFFFFFFFFA4001FF0ull;               /* sp */
    c->gpr[11] = 0xFFFFFFFFA4000040ull;               /* t3 */
    c->gpr[31] = 0xFFFFFFFFA4001550ull;               /* ra */
    c->cp0_status = 0x34000000u;
    /* The boot leaves Cause.CE at 3 (krom's COP0Cause test reads it). */
    c->cp0_cause = 0x30000000u;
    c->cp0_compare = 0;
    n64_cpu_update_mode(c);
}

/* IPL1 and IPL2, then the cartridge's IPL3 from DMEM. */
static void boot_ipl3(N64Machine *m)
{
    memcpy(m->rsp.dmem, m->rom, 0x1000);
    m->rsp.status |= N64_SP_STATUS_HALT;
    ipl2_registers(m);
    /* IPL2 leaves the CIC's info byte and seeds visible in PIF RAM and
     * has locked out the PIF ROM; the handshake continues with IPL3. */
    m->pif_ram[0x25] = 0xB4u;
    m->pif_ram[0x26] = (uint8_t)m->cart.cic_seed;
    m->pif_ram[0x27] = (uint8_t)m->cart.cic_seed;
    m->pif_rom_lockout = true;
    m->pif_state = N64_PIF_WAIT_GET_CHECKSUM;
    m->cpu.pc = 0xFFFFFFFFA4000040ull;
}

void n64_boot(N64Machine *m)
{
    if (m->cart.ipl3_known && !m->hle_boot) {
        boot_ipl3(m);
        return;
    }
    N64Cpu *c = &m->cpu;
    const N64Cart *cart = &m->cart;

    uint32_t entry = cart->entry;
    if (cart->cic == 6103) entry -= 0x100000u;
    if (cart->cic == 6106) entry -= 0x200000u;
    const uint32_t load = entry & 0x1FFFFFFFu;
    for (uint32_t i = 0; i < 0x100000u; i++) {
        uint32_t src = 0x1000u + i;
        if (load + i >= m->rdram_size) break;
        m->rdram[load + i] = src < m->rom_size ? m->rom[src] : 0;
    }

    memcpy(m->rsp.dmem + 0x40, m->rom + 0x40, 0x1000 - 0x40);
    m->rsp.status |= N64_SP_STATUS_HALT;

    n64_put_be32(m->rdram + (cart->cic == 6105 ? 0x3F0u : 0x318u), m->rdram_size);

    n64_rdram_configure(m);
    ipl2_registers(m);
    c->pc = (uint64_t)(int64_t)(int32_t)entry;
}
