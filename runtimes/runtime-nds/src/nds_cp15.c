/*
 * CP15, the ARM9's system control coprocessor, as the DS has it: an ID
 * block, the control register, the protection unit's region and permission
 * registers, the cache commands and the two tightly coupled memories
 * (GBATEK, "ARM CP15 system control coprocessor").
 *
 * The protection unit and the caches are stored but not enforced. What has
 * an effect here: the vector base, the "pre-ARMv5" bit that stops loads
 * into R15 changing the instruction set, the TCM registers (the bus maps
 * the memories from them) and wait-for-interrupt.
 */
#include "nds_bus.h"
#include "nds_cpu.h"

/* Control register bits software can change; the rest are fixed. */
#define CONTROL_WRITABLE 0x000FF085u

/* The legacy access-permission registers hold two bits per region; the
 * extended ones hold four. They are views of the same register. */
static uint32_t legacy_from_extended(uint32_t extended)
{
    uint32_t legacy = 0;
    for (int region = 0; region < 8; region++) {
        legacy |= ((extended >> (region * 4)) & 3u) << (region * 2);
    }
    return legacy;
}

static uint32_t extended_from_legacy(uint32_t legacy)
{
    uint32_t extended = 0;
    for (int region = 0; region < 8; region++) {
        extended |= ((legacy >> (region * 2)) & 3u) << (region * 4);
    }
    return extended;
}

uint32_t nds9_cp15_read(const NdsCpu *cpu, uint32_t id)
{
    const NdsCp15 *cp = &cpu->cp15;

    switch (id) {
    case 0x000: /* main ID; the unused opcode2 values read the same */
    case 0x003:
    case 0x004:
    case 0x005:
    case 0x006:
    case 0x007:
        return 0x41059461u;
    case 0x001: return 0x0F0D2112u; /* cache type */
    case 0x002: return 0x00140180u; /* TCM size */

    case 0x100: return cp->control;
    case 0x200: return cp->dcache_regions;
    case 0x201: return cp->icache_regions;
    case 0x300: return cp->write_buffer;

    case 0x500: return legacy_from_extended(cp->data_access);
    case 0x501: return legacy_from_extended(cp->code_access);
    case 0x502: return cp->data_access;
    case 0x503: return cp->code_access;

    case 0x900: return cp->dcache_lockdown;
    case 0x901: return cp->icache_lockdown;
    case 0x910: return cp->dtcm;
    case 0x911: return cp->itcm;

    case 0xD01:
    case 0xD11:
        return cp->process_id;
    default:
        break;
    }

    /* C6,C0-C7,0 and ,1: the protection regions. */
    if ((id & 0xF8Eu) == 0x600u) return cp->region[(id >> 4) & 7u];
    return 0;
}

void nds9_cp15_write(NdsCpu *cpu, uint32_t id, uint32_t value)
{
    NdsCp15 *cp = &cpu->cp15;

    switch (id) {
    case 0x100:
        cp->control = (cp->control & ~CONTROL_WRITABLE) | (value & CONTROL_WRITABLE);
        cpu->vector_base = (cp->control & NDS_CP15_HIGH_VECTORS) ? 0xFFFF0000u : 0u;
        nds9_bus_cp15_changed(cpu);
        return;

    case 0x200: cp->dcache_regions = value; return;
    case 0x201: cp->icache_regions = value; return;
    case 0x300: cp->write_buffer = value; return;

    case 0x500: cp->data_access = extended_from_legacy(value); return;
    case 0x501: cp->code_access = extended_from_legacy(value); return;
    case 0x502: cp->data_access = value; return;
    case 0x503: cp->code_access = value; return;

    case 0x704: /* wait for interrupt */
    case 0x782:
        nds9_bus_wait_for_interrupt(cpu);
        return;

    case 0x900: cp->dcache_lockdown = value; return;
    case 0x901: cp->icache_lockdown = value; return;
    case 0x910:
        cp->dtcm = value & 0xFFFFF03Eu;
        nds9_bus_cp15_changed(cpu);
        return;
    case 0x911:
        cp->itcm = value & 0x0000003Eu;
        nds9_bus_cp15_changed(cpu);
        return;

    case 0xD01:
    case 0xD11:
        cp->process_id = value;
        return;
    default:
        break;
    }

    if ((id & 0xF8Eu) == 0x600u) {
        cp->region[(id >> 4) & 7u] = value;
        return;
    }
    /* Everything else is a cache command (C7) or a test register (C15):
     * nothing to do without caches. */
}
