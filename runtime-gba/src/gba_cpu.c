/*
 * ARM7TDMI state management: register banking, CPSR writes, exceptions,
 * reset and the fetch/execute step.
 *
 * Banking follows the hardware model the test vectors were generated from:
 * r[] always holds the registers visible in the current mode, and each
 * bank slot holds the values last saved when that mode was left. The slot
 * of the current mode is therefore stale until the mode changes again.
 */
#include "gba_cpu.h"

#include <string.h>

#include "gba_bus.h"

int gba_cpu_bank_for_mode(uint32_t mode)
{
    switch (mode & 0x1Fu) {
    case GBA_MODE_FIQ: return GBA_BANK_FIQ;
    case GBA_MODE_SVC: return GBA_BANK_SVC;
    case GBA_MODE_ABT: return GBA_BANK_ABT;
    case GBA_MODE_IRQ: return GBA_BANK_IRQ;
    case GBA_MODE_UND: return GBA_BANK_UND;
    default: return GBA_BANK_NONE;
    }
}

void gba_cpu_switch_mode(GbaCpu *cpu, uint32_t new_mode)
{
    int old_bank = gba_cpu_bank_for_mode(cpu->cpsr);
    int new_bank = gba_cpu_bank_for_mode(new_mode);

    cpu->cpsr = (cpu->cpsr & ~0x1Fu) | (new_mode & 0x1Fu);
    if (old_bank == new_bank) return;

    if (old_bank == GBA_BANK_FIQ) {
        memcpy(cpu->bank_r8_r12[1], &cpu->r[8], sizeof cpu->bank_r8_r12[1]);
        memcpy(&cpu->r[8], cpu->bank_r8_r12[0], sizeof cpu->bank_r8_r12[0]);
    } else if (new_bank == GBA_BANK_FIQ) {
        memcpy(cpu->bank_r8_r12[0], &cpu->r[8], sizeof cpu->bank_r8_r12[0]);
        memcpy(&cpu->r[8], cpu->bank_r8_r12[1], sizeof cpu->bank_r8_r12[1]);
    }

    cpu->bank_r13[old_bank] = cpu->r[13];
    cpu->bank_r14[old_bank] = cpu->r[14];
    cpu->r[13] = cpu->bank_r13[new_bank];
    cpu->r[14] = cpu->bank_r14[new_bank];
}

void gba_cpu_set_cpsr(GbaCpu *cpu, uint32_t value)
{
    gba_cpu_switch_mode(cpu, value & 0x1Fu);
    cpu->cpsr = value & ~GBA_CPSR_FLAGS_MASK;
    cpu->flag_n = (uint8_t)((value >> 31) & 1u);
    cpu->flag_z = (uint8_t)((value >> 30) & 1u);
    cpu->flag_c = (uint8_t)((value >> 29) & 1u);
    cpu->flag_v = (uint8_t)((value >> 28) & 1u);
}

uint32_t *gba_cpu_current_spsr(GbaCpu *cpu)
{
    return &cpu->spsr[gba_cpu_bank_for_mode(cpu->cpsr)];
}

bool gba_cpu_has_spsr(const GbaCpu *cpu)
{
    return gba_cpu_bank_for_mode(cpu->cpsr) != GBA_BANK_NONE;
}

uint32_t gba_cpu_read_spsr(const GbaCpu *cpu)
{
    int bank = gba_cpu_bank_for_mode(cpu->cpsr);
    return bank == GBA_BANK_NONE ? gba_cpu_get_cpsr(cpu) : cpu->spsr[bank];
}

uint32_t gba_cpu_get_user_reg(const GbaCpu *cpu, int reg)
{
    int bank = gba_cpu_bank_for_mode(cpu->cpsr);
    if (reg >= 8 && reg <= 12 && bank == GBA_BANK_FIQ) return cpu->bank_r8_r12[0][reg - 8];
    if ((reg == 13 || reg == 14) && bank != GBA_BANK_NONE) {
        return reg == 13 ? cpu->bank_r13[GBA_BANK_NONE] : cpu->bank_r14[GBA_BANK_NONE];
    }
    return cpu->r[reg];
}

void gba_cpu_set_user_reg(GbaCpu *cpu, int reg, uint32_t value)
{
    int bank = gba_cpu_bank_for_mode(cpu->cpsr);
    if (reg >= 8 && reg <= 12 && bank == GBA_BANK_FIQ) {
        cpu->bank_r8_r12[0][reg - 8] = value;
    } else if ((reg == 13 || reg == 14) && bank != GBA_BANK_NONE) {
        if (reg == 13) cpu->bank_r13[GBA_BANK_NONE] = value;
        else cpu->bank_r14[GBA_BANK_NONE] = value;
    } else {
        cpu->r[reg] = value;
    }
}

void gba_cpu_exception(GbaCpu *cpu, uint32_t vector, uint32_t mode, uint32_t return_addr)
{
    uint32_t old_cpsr = gba_cpu_get_cpsr(cpu);
    gba_cpu_switch_mode(cpu, mode);
    cpu->spsr[gba_cpu_bank_for_mode(mode)] = old_cpsr;
    cpu->r[14] = return_addr;
    cpu->cpsr = (cpu->cpsr & ~GBA_CPSR_T) | GBA_CPSR_I;
    if (mode == GBA_MODE_FIQ || vector == GBA_VECTOR_RESET) cpu->cpsr |= GBA_CPSR_F;
    cpu->r[15] = vector + 8;
    cpu->branched = true;
}

void gba_cpu_reset(GbaCpu *cpu, bool skip_bios)
{
    void *user = cpu->user;
    memset(cpu, 0, sizeof *cpu);
    cpu->user = user;
    if (skip_bios) {
        /* State the BIOS leaves behind when it jumps to the cartridge. */
        cpu->cpsr = GBA_MODE_SYS;
        cpu->bank_r13[GBA_BANK_SVC] = 0x03007FE0u;
        cpu->bank_r13[GBA_BANK_IRQ] = 0x03007FA0u;
        cpu->r[13] = 0x03007F00u;
        cpu->r[15] = 0x08000000u + 8;
    } else {
        cpu->cpsr = GBA_MODE_SVC | GBA_CPSR_I | GBA_CPSR_F;
        cpu->r[15] = GBA_VECTOR_RESET + 8;
    }
}

void gba_cpu_refill(GbaCpu *cpu)
{
    uint32_t pc = gba_cpu_pc(cpu);
    if (cpu->cpsr & GBA_CPSR_T) {
        cpu->pipe[0] = gba_bus_read16(cpu, pc, GBA_ACCESS_CODE | GBA_ACCESS_NSEQ);
        cpu->pipe[1] = gba_bus_read16(cpu, pc + 2, GBA_ACCESS_CODE | GBA_ACCESS_SEQ);
    } else {
        cpu->pipe[0] = gba_bus_read32(cpu, pc, GBA_ACCESS_CODE | GBA_ACCESS_NSEQ);
        cpu->pipe[1] = gba_bus_read32(cpu, pc + 4, GBA_ACCESS_CODE | GBA_ACCESS_SEQ);
    }
}

void gba_cpu_step(GbaCpu *cpu)
{
    uint32_t op = cpu->pipe[0];
    cpu->pipe[0] = cpu->pipe[1];
    if (cpu->cpsr & GBA_CPSR_T) {
        cpu->pipe[1] = gba_bus_read16(cpu, cpu->r[15] & ~1u, GBA_ACCESS_CODE | GBA_ACCESS_SEQ);
        gba_cpu_execute_thumb(cpu, (uint16_t)op);
    } else {
        cpu->pipe[1] = gba_bus_read32(cpu, cpu->r[15] & ~3u, GBA_ACCESS_CODE | GBA_ACCESS_SEQ);
        gba_cpu_execute_arm(cpu, op);
    }
    if (cpu->branched) gba_cpu_refill(cpu);
}

bool gba_cpu_irq(GbaCpu *cpu)
{
    if (cpu->cpsr & GBA_CPSR_I) return false;
    /* LR_irq = address of the next instruction + 4, so SUBS PC, LR, #4
     * resumes it in either state. */
    gba_cpu_exception(cpu, GBA_VECTOR_IRQ, GBA_MODE_IRQ, gba_cpu_pc(cpu) + 4);
    gba_cpu_refill(cpu);
    return true;
}
