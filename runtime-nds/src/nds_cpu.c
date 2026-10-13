/*
 * State management common to both DS processors: register banking, CPSR
 * writes, exception entry and reset.
 *
 * Banking follows the hardware model the test vectors were generated from:
 * r[] always holds the registers visible in the current mode, and each
 * bank slot holds the values last saved when that mode was left. The slot
 * of the current mode is therefore stale until the mode changes again.
 */
#include "nds_cpu.h"

#include <string.h>

int nds_cpu_bank_for_mode(uint32_t mode)
{
    switch (mode & 0x1Fu) {
    case NDS_MODE_FIQ: return NDS_BANK_FIQ;
    case NDS_MODE_SVC: return NDS_BANK_SVC;
    case NDS_MODE_ABT: return NDS_BANK_ABT;
    case NDS_MODE_IRQ: return NDS_BANK_IRQ;
    case NDS_MODE_UND: return NDS_BANK_UND;
    default: return NDS_BANK_NONE;
    }
}

void nds_cpu_switch_mode(NdsCpu *cpu, uint32_t new_mode)
{
    int old_bank = nds_cpu_bank_for_mode(cpu->cpsr);
    int new_bank = nds_cpu_bank_for_mode(new_mode);

    cpu->cpsr = (cpu->cpsr & ~0x1Fu) | (new_mode & 0x1Fu);
    if (old_bank == new_bank) return;

    if (old_bank == NDS_BANK_FIQ) {
        memcpy(cpu->bank_r8_r12[1], &cpu->r[8], sizeof cpu->bank_r8_r12[1]);
        memcpy(&cpu->r[8], cpu->bank_r8_r12[0], sizeof cpu->bank_r8_r12[0]);
    } else if (new_bank == NDS_BANK_FIQ) {
        memcpy(cpu->bank_r8_r12[0], &cpu->r[8], sizeof cpu->bank_r8_r12[0]);
        memcpy(&cpu->r[8], cpu->bank_r8_r12[1], sizeof cpu->bank_r8_r12[1]);
    }

    cpu->bank_r13[old_bank] = cpu->r[13];
    cpu->bank_r14[old_bank] = cpu->r[14];
    cpu->r[13] = cpu->bank_r13[new_bank];
    cpu->r[14] = cpu->bank_r14[new_bank];
}

void nds_cpu_set_cpsr(NdsCpu *cpu, uint32_t value)
{
    nds_cpu_switch_mode(cpu, value & 0x1Fu);
    cpu->cpsr = value & ~NDS_CPSR_FLAGS_MASK;
    cpu->flag_n = (uint8_t)((value >> 31) & 1u);
    cpu->flag_z = (uint8_t)((value >> 30) & 1u);
    cpu->flag_c = (uint8_t)((value >> 29) & 1u);
    cpu->flag_v = (uint8_t)((value >> 28) & 1u);
}

uint32_t *nds_cpu_current_spsr(NdsCpu *cpu)
{
    return &cpu->spsr[nds_cpu_bank_for_mode(cpu->cpsr)];
}

bool nds_cpu_has_spsr(const NdsCpu *cpu)
{
    return nds_cpu_bank_for_mode(cpu->cpsr) != NDS_BANK_NONE;
}

uint32_t nds_cpu_read_spsr(const NdsCpu *cpu)
{
    int bank = nds_cpu_bank_for_mode(cpu->cpsr);
    return bank == NDS_BANK_NONE ? nds_cpu_get_cpsr(cpu) : cpu->spsr[bank];
}

uint32_t nds_cpu_get_user_reg(const NdsCpu *cpu, int reg)
{
    int bank = nds_cpu_bank_for_mode(cpu->cpsr);
    if (reg >= 8 && reg <= 12 && bank == NDS_BANK_FIQ) return cpu->bank_r8_r12[0][reg - 8];
    if ((reg == 13 || reg == 14) && bank != NDS_BANK_NONE) {
        return reg == 13 ? cpu->bank_r13[NDS_BANK_NONE] : cpu->bank_r14[NDS_BANK_NONE];
    }
    return cpu->r[reg];
}

void nds_cpu_set_user_reg(NdsCpu *cpu, int reg, uint32_t value)
{
    int bank = nds_cpu_bank_for_mode(cpu->cpsr);
    if (reg >= 8 && reg <= 12 && bank == NDS_BANK_FIQ) {
        cpu->bank_r8_r12[0][reg - 8] = value;
    } else if ((reg == 13 || reg == 14) && bank != NDS_BANK_NONE) {
        if (reg == 13) cpu->bank_r13[NDS_BANK_NONE] = value;
        else cpu->bank_r14[NDS_BANK_NONE] = value;
    } else {
        cpu->r[reg] = value;
    }
}

void nds_cpu_exception(NdsCpu *cpu, uint32_t vector, uint32_t mode, uint32_t return_addr)
{
    uint32_t old_cpsr = nds_cpu_get_cpsr(cpu);
    nds_cpu_switch_mode(cpu, mode);
    cpu->spsr[nds_cpu_bank_for_mode(mode)] = old_cpsr;
    cpu->r[14] = return_addr;
    cpu->cpsr = (cpu->cpsr & ~NDS_CPSR_T) | NDS_CPSR_I;
    if (mode == NDS_MODE_FIQ || vector == NDS_VECTOR_RESET) cpu->cpsr |= NDS_CPSR_F;
    cpu->r[15] = cpu->vector_base + vector + 8;
    cpu->branched = true;
}

static void reset_common(NdsCpu *cpu)
{
    void *user = cpu->user;
    memset(cpu, 0, sizeof *cpu);
    cpu->user = user;
    cpu->cpsr = NDS_MODE_SVC | NDS_CPSR_I | NDS_CPSR_F;
    /* A mode field of zero names no mode; give every SPSR a valid one. */
    for (int bank = 0; bank < NDS_BANK_COUNT; bank++) cpu->spsr[bank] = NDS_MODE_USR;
}

void nds9_cpu_reset(NdsCpu *cpu)
{
    reset_common(cpu);
    /* Bits 3-6 of the control register always read as one. The DS wires
     * the ARM9 to start with its vectors at 0xFFFF0000. */
    cpu->cp15.control = 0x00000078u | NDS_CP15_HIGH_VECTORS;
    cpu->vector_base = 0xFFFF0000u;
    cpu->r[15] = cpu->vector_base + NDS_VECTOR_RESET + 8;
}

void nds7_cpu_reset(NdsCpu *cpu)
{
    reset_common(cpu);
    cpu->vector_base = 0;
    cpu->r[15] = NDS_VECTOR_RESET + 8;
}
