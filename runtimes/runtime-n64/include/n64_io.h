/*
 * The RCP's interfaces as the machine's run loop and bus see them
 * (n64_io.c, n64_pif.c, n64_cart.c, n64_vi.c). Internal to the runtime.
 */
#ifndef N64_IO_H
#define N64_IO_H

#include "n64.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register blocks: `offset` is the byte offset in the block. */
uint32_t n64_mi_read(N64Machine *m, uint32_t offset);
void n64_mi_write(N64Machine *m, uint32_t offset, uint32_t value);
uint32_t n64_vi_read(N64Machine *m, uint32_t offset);
void n64_vi_write(N64Machine *m, uint32_t offset, uint32_t value);
uint32_t n64_ai_read(N64Machine *m, uint32_t offset);
void n64_ai_write(N64Machine *m, uint32_t offset, uint32_t value);
uint32_t n64_pi_read(N64Machine *m, uint32_t offset);
void n64_pi_write(N64Machine *m, uint32_t offset, uint32_t value);
/* RI and the RDRAM chips (n64_rdram.c). */
uint32_t n64_ri_read(N64Machine *m, uint32_t offset);
void n64_ri_write(N64Machine *m, uint32_t offset, uint32_t value);
void n64_rdram_power(N64Machine *m);
void n64_rdram_configure(N64Machine *m);
void n64_rdram_update_fast(N64Machine *m);
bool n64_rdram_translate(N64Machine *m, uint32_t pa, uint32_t *out, unsigned *chip_index);
uint32_t n64_rdram_degrade(N64Machine *m, unsigned chip_index, uint32_t value);
uint32_t n64_rdram_reg_read(N64Machine *m, uint32_t pa);
void n64_rdram_reg_write(N64Machine *m, uint32_t pa, uint32_t value, unsigned repeat);
uint32_t n64_si_read(N64Machine *m, uint32_t offset);
void n64_si_write(N64Machine *m, uint32_t offset, uint32_t value);
uint32_t n64_sp_read(N64Machine *m, uint32_t paddr);
void n64_sp_write(N64Machine *m, uint32_t paddr, uint32_t value);

/* Events. */
void n64_vi_line(N64Machine *m, uint64_t at);
uint64_t n64_vi_line_cycles(N64Machine *m);
void n64_ai_sample(N64Machine *m, uint64_t at);
uint64_t n64_ai_period(N64Machine *m);
void n64_pi_dma_done(N64Machine *m);
void n64_si_dma_done(N64Machine *m);
void n64_rsp_started(N64Machine *m);

/* The cartridge bus (PI domains 1 and 2): select an address, then move
 * halfwords, the way the PI and the CPU's uncached accesses reach it. */
void n64_cart_select(N64Machine *m, uint32_t paddr);
uint16_t n64_cart_read_half(N64Machine *m);
void n64_cart_write_half(N64Machine *m, uint16_t value);
void n64_flash_reset(N64Machine *m);
void n64_flash_done(N64Machine *m);

/* The PIF: DMA in and out of its RAM, and the joybus. */
void n64_pif_dma_read(N64Machine *m);  /* PIF RAM -> RDRAM */
void n64_pif_dma_write(N64Machine *m); /* RDRAM -> PIF RAM */
void n64_pif_cpu_written(N64Machine *m); /* after the CPU stores to PIF RAM */
uint64_t n64_pif_read_cycles(N64Machine *m);
uint8_t n64_pif_data_crc(const uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif /* N64_IO_H */
