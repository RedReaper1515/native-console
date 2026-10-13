/*
 * The RSP: the Reality Signal Processor's scalar unit (a MIPS R4000 subset
 * with 32-bit registers and a 12-bit PC), its vector unit, its 4 KiB of
 * data memory (DMEM) and 4 KiB of instruction memory (IMEM), its DMA
 * engine and the SP register block the CPU sees.
 *
 * This is a library with a link-time-free seam: the machine owns an
 * N64Rsp, points `rdram` at its RDRAM, and supplies the callbacks below.
 * The RSP never calls into the machine any other way, so it is tested on
 * its own (tests/n64/n64_rsp_test.c).
 *
 * Seam contract (docs/N64-PLAN.md, "The RSP"):
 *
 *  - DMEM and IMEM hold big-endian bytes, exactly as the console does.
 *    The CPU reads and writes them directly through the machine's bus
 *    (0x04000000-0x04001FFF), so their layout is part of the contract.
 *  - n64_rsp_run() runs whole instructions until the RSP halts or `budget`
 *    RSP cycles have been used, and returns how many were used. It is
 *    deterministic: the same calls give the same results. One instruction
 *    costs one RSP cycle (the RSP runs at 2/3 of the CPU clock; the machine
 *    does that conversion, not the RSP).
 *  - The machine catches the RSP up before the CPU reads or writes any SP
 *    register, DMEM or IMEM, so a register read never sees the future.
 *  - Register numbers follow the RSP's COP0 numbering: 0-7 are the SP
 *    registers at 0x04040000 + 4 * reg, which is also what the CPU sees;
 *    N64_SP_REG_PC (the SP_PC register at 0x04080000) is a separate
 *    number. COP0 registers 8-15 are the RDP's command registers; the RSP
 *    reaches them through dp_read and dp_write.
 *  - DMA is performed when the length register is written. Lengths, skips
 *    and counts follow the hardware: (len & 0xFF8) + 8 bytes per row,
 *    rows = (len >> 12 & 0xFF) + 1, skip = len >> 20 & 0xFF8 bytes added
 *    to the RDRAM address between rows; DMEM/IMEM addresses are 8-byte
 *    aligned and wrap within their 4 KiB (bit 12 of the memory address
 *    selects IMEM and never changes); RDRAM addresses are 8-byte aligned
 *    and 24 bits wide, and reads beyond rdram_size give zeroes, writes
 *    there are dropped. As on the console, the address and length
 *    registers are double-buffered: writes set the pending values the next
 *    transfer starts from, reads return the current ones, which a finished
 *    transfer leaves just past the last byte it moved, with the length
 *    field at 0xFF8 and the count at 0. DMA_FULL and DMA_BUSY read 0, as
 *    every transfer has finished by the time anything can look.
 *  - `interrupt(user, level)` drives the MI's SP interrupt line: level 1
 *    on SP_STATUS "set intr" or on BREAK with interrupt-on-break set;
 *    level 0 on "clear intr".
 *  - BREAK sets HALT and BROKE; the PC is left at the instruction after it
 *    (or at the branch target when it sits in a taken branch's delay slot),
 *    so clearing HALT resumes there. With SSTEP set, the RSP halts after
 *    every instruction it runs. SP_PC reads the address of the next
 *    instruction; writing it also cancels a pending branch.
 *  - Instruction semantics live in n64_rsp_impl.h, one force-inlined
 *    handler per instruction, so a recompiler can emit calls to the same
 *    code the interpreter runs.
 *  - IMEM may be written at any time between n64_rsp_run() calls, by the
 *    bus directly or by DMA, and by a DMA the RSP starts itself; nobody
 *    has to tell the RSP. The interpreter keeps each IMEM word's decoding,
 *    and before running a word compares it with the one it decoded, so a
 *    stale decoding is never run. (The RSP's own loads and stores, scalar
 *    and vector, reach DMEM only and wrap inside it.)
 *  - No host pointer other than the `links` member lives in the struct;
 *    save states scrub `links` on save and restore it on load.
 */
#ifndef N64_RSP_H
#define N64_RSP_H

#include "n64_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SP register numbers (CPU address 0x04040000 + 4 * n; RSP COP0 0-7). */
enum {
    N64_SP_REG_MEM_ADDR = 0,
    N64_SP_REG_DRAM_ADDR = 1,
    N64_SP_REG_RD_LEN = 2,
    N64_SP_REG_WR_LEN = 3,
    N64_SP_REG_STATUS = 4,
    N64_SP_REG_DMA_FULL = 5,
    N64_SP_REG_DMA_BUSY = 6,
    N64_SP_REG_SEMAPHORE = 7,
    N64_SP_REG_PC = 8, /* 0x04080000 */
};

/* SP_STATUS read bits. */
#define N64_SP_STATUS_HALT 0x0001u
#define N64_SP_STATUS_BROKE 0x0002u
#define N64_SP_STATUS_DMA_BUSY 0x0004u
#define N64_SP_STATUS_DMA_FULL 0x0008u
#define N64_SP_STATUS_IO_FULL 0x0010u
#define N64_SP_STATUS_SSTEP 0x0020u
#define N64_SP_STATUS_INTR_BREAK 0x0040u
#define N64_SP_STATUS_SIG0 0x0080u /* SIG0..SIG7 are bits 7..14 */

typedef struct N64RspLinks {
    uint8_t *rdram;      /* big-endian RDRAM */
    uint32_t rdram_size; /* bytes */
    void *user;
    void (*interrupt)(void *user, int level);
    /* RDP command registers 0-7 (DPC_START ... DPC_TMEM), seen through the
     * RSP's COP0 registers 8-15. */
    uint32_t (*dp_read)(void *user, unsigned reg);
    void (*dp_write)(void *user, unsigned reg, uint32_t value);
    /* Optional: DMA wrote RDRAM [addr, addr + size), so recompiled code
     * compiled from those bytes must be checked again. */
    void (*ram_written)(void *user, uint32_t addr, uint32_t size);
} N64RspLinks;

/* The vector unit: 32 registers of 8 16-bit lanes, the accumulator (48
 * bits per lane, kept as three 16-bit slices), the flag registers and the
 * divide unit's state. Lane 0 is the most significant (big-endian) lane. */
typedef struct N64RspVu {
    uint16_t v[32][8];
    uint16_t acc_h[8], acc_m[8], acc_l[8];
    /* The flag registers hold one bit per lane: bit n is lane n, which is
     * also how CFC2 and CTC2 lay them out (VCO = vco_hi << 8 | vco_lo). */
    uint16_t vco_lo, vco_hi; /* carry, not-equal */
    uint16_t vcc_lo, vcc_hi; /* compare */
    uint8_t vce;
    int16_t div_in, div_out;
    uint8_t div_dp;
} N64RspVu;

/* What the interpreter keeps of an IMEM word it has decoded: the word and
 * the instruction number the decoder gave it. */
typedef struct N64RspDecoded {
    uint32_t op;
    uint32_t id;
} N64RspDecoded;

typedef struct N64Rsp {
    uint8_t dmem[0x1000];
    uint8_t imem[0x1000];

    /* Scalar unit. r[0] is always 0. */
    uint32_t r[32];
    uint32_t pc;      /* 12 bits, word aligned: the instruction to run next */
    uint32_t next_pc; /* where the instruction after this one comes from */
    /* Scratch for the instruction being run: a branch or jump that is taken
     * sets these, and the step then makes branch_target the next_pc of the
     * delay slot. Both are 0 between instructions. */
    uint8_t branch_pending;
    uint32_t branch_target;

    N64RspVu vu;

    /* SP registers. The DMA registers are double-buffered (see above):
     * mem_addr, dram_addr, rd_len and wr_len are the values last written,
     * dma_mem_addr, dma_dram_addr and dma_len what the CPU reads back. */
    uint32_t mem_addr;  /* bit 12 selects IMEM */
    uint32_t dram_addr;
    uint32_t rd_len, wr_len;
    uint32_t dma_mem_addr, dma_dram_addr, dma_len;
    uint32_t status;    /* N64_SP_STATUS_* bits; DMA and IO bits stay 0 */
    uint32_t semaphore;

    uint64_t cycles;       /* RSP cycles run since reset */
    uint64_t instructions; /* instructions run since reset */

    /* The interpreter's decodings, one per IMEM word. They are derived from
     * IMEM and check themselves (see above), so a save state may leave them
     * out; what they must not hold is a number that is not its word's. All
     * zeroes is valid. */
    N64RspDecoded decoded[0x400];

    N64RspLinks links;
} N64Rsp;

/* Power-on state: halted, PC 0, memories and registers cleared. Keeps
 * `links`. */
void n64_rsp_reset(N64Rsp *rsp);

/* Run until halted or `budget` cycles are used; returns the cycles used
 * (0 when halted on entry). */
uint64_t n64_rsp_run(N64Rsp *rsp, uint64_t budget);

static inline bool n64_rsp_halted(const N64Rsp *rsp) { return (rsp->status & N64_SP_STATUS_HALT) != 0; }

/* SP registers as the CPU sees them (N64_SP_REG_*). Reading SEMAPHORE sets
 * it; writing it clears it. */
uint32_t n64_rsp_reg_read(N64Rsp *rsp, unsigned reg);
void n64_rsp_reg_write(N64Rsp *rsp, unsigned reg, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* N64_RSP_H */
