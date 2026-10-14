# Audit: Game Boy assembly-to-C nuances vs Native Console

Checked 2026-09-09 on the fork (now `RedReaper1515/native-console`, main with
the bank-resolution fix). Every claim below is backed either by a test ROM
run **in the shipped free-running native mode** (not `--differential`, which
single-steps) or by a cited code path. Test ROMs: Blargg (retrio/gb-test-roms)
and Mooneye MTS 2024-09-26, recompiled by `build/bin/gbrecomp.exe` and built
with MSVC on Windows.

Legend: **OK** handled and verified · **OK\*** handled with a caveat ·
**FIXED** was wrong, fixed in this fork · **GAP** not handled.

| # | Nuance | Status | Evidence |
|---|--------|--------|----------|
| 1 | 8-bit vs 16-bit operations, C integer promotion | OK | ALU helpers in `runtimes/runtime-gb/src/gbrt.c` take and return `uint8_t`/`uint16_t` and compute flags from the widened intermediate before truncating (`gb_add8` 3450, `gb_add16`, `gb_add_sp` 3551). Blargg `cpu_instrs` 01-11 pass, `mem_timing` 1/2 pass. |
| 2 | CPU flags carried to later branches | OK | Flags are four explicit bytes (`f_z f_n f_h f_c`, `gbrt.h:283-286`); every ALU op writes them; branches are emitted as `if (ctx->f_c)` etc., never as C comparisons on the result. `cpu_instrs` pass. |
| 3 | Half-carry | OK | Computed from bit 3 (8-bit) / bit 11 (16-bit) / bit 3 of the low byte (`ADD SP,e`, `LD HL,SP+e`). Mooneye `instr/daa`, Blargg `cpu_instrs` 04/05/09/11 pass. |
| 4 | Carry and borrow (ADC, SBC, CCF, …) | OK | `cpu_instrs` 01-special and 09-op r,r pass. |
| 5 | Signed vs unsigned comparisons | OK | Conditional jumps test the flag bits (`c_emitter.cpp` JUMP_CC 1760-1860, CALL_CC 1960-2058); no `<`/`>` on register values is ever emitted. |
| 6 | 16-bit register pairs aliasing their bytes | OK | `GBContext` stores AF/BC/DE/HL as `union { struct {uint8_t lo, hi;}; uint16_t pair; }` (`gbrt.h:261-275`). |
| 7 | Memory-mapped I/O | FIXED | All generated loads/stores go through the bus (`gbrt_read8_impl`/`gbrt_write8_impl`), which dispatches to PPU, APU, timer, DMA, joypad and mapper. The only fast paths are WRAM/HRAM and they are gated on DMA state. Fixed today: the fast path now also yields during the DMA start-up window (`!dma.active && !dma.pending`), matching the bus predicate. |
| 8 | LDH / `$FF00` accesses | OK | Same bus; `LDH` is emitted as `gb_read8(ctx, GB_HRAM_BASE + n)` / `gb_write8`. Mooneye `if_ie_registers`, `oam_dma/reg_read` pass. |
| 9 | VRAM / OAM access by PPU mode | OK\* | OAM blocked in modes 2/3 (Mooneye `bits/mem_oam` pass); VRAM reads block on internal-or-visible mode 3, writes on visible mode only. That asymmetry is what hardware does: making writes block early fails Mooneye `ppu/lcdon_write_timing-GS`, which passes as written. Blargg `oam_bug` fails 5 of 8 sub-tests: the DMG OAM-corruption quirk on 16-bit inc/dec near OAM is only partially modelled. |
| 10 | OAM DMA is hardware, not a store | FIXED | `$FF46` write starts a modelled transfer with start-up delay, restart and bus conflicts. Mooneye `oam_dma/basic`, `reg_read`, `sources-GS`, `oam_dma_start`, `oam_dma_restart`, `oam_dma_timing` pass. Fixed today: on DMG the bus-conflict predicate blocked `$FF00-$FF7F` and `$FFFF` too (`gb_oam_dma_blocks_cpu_addr`), so an ISR or `halt` during DMA read I/O and IE as `$FF`; those live on the CPU-internal bus and are now reachable. |
| 11 | EI / DI / HALT / STOP / interrupts | FIXED | 20-cycle interrupt entry with IE re-check and cancellation to `$0000`; EI one-instruction delay via `ime_pending`. Mooneye `ei_sequence`, `ei_timing`, `di_timing-GS`, `rapid_di_ei`, `intr_timing`, `ie_push`, `if_ie_registers`, `halt_ime0_ei`, `halt_ime0_nointr_timing`, `halt_ime1_timing`, `halt_ime1_timing2-GS` all pass. Fixed today: entering STOP and the CGB speed switch now reset DIV (`gb_stop`), and a joypad press only leaves HALT when `IE & IF` is non-zero (`platform_sdl.cpp`, `differential.c`). Remaining caveat: the PPU and timer keep clocking during DMG STOP. |
| 12 | HALT bug | OK | `gbrt_execute_halt` detects IME=0 with a pending interrupt and sets `halt_bug`; `gb_step` (gbrt.c:5452) then routes the next instruction through the interpreter so the byte is executed twice. Blargg `halt_bug` passes on screen in native mode; `tests/test_halt_bug.c` covers five scenarios. |
| 13 | DIV / TIMA / TMA / TAC driven by time | OK | Timer ticks from the scheduler, not from game code. Mooneye `timer/div_write`, `rapid_toggle`, `tim00/01/10/11`, `tim00_div_trigger`, `tim11_div_trigger`, `tima_reload`, `tima_write_reloading`, `tma_write_reloading`, `div_timing` pass. |
| 14 | PPU timing / code on specific scanlines | FIXED | Dot-serial PPU (`runtimes/runtime-gb/src/ppu.c`) advanced by CPU cycles. Mooneye `ppu/intr_2_0_timing`, `stat_irq_blocking`, `lcdon_timing-GS`, `vblank_stat_intr-GS` pass. Fixed today: the WY == LY window latch is sampled when mode 2 begins (`ppu_latch_window_y`), not at mode-3 start. Also pass: `intr_2_mode0_timing`, `intr_2_mode3_timing`, `intr_2_oam_ok_timing`, `intr_1_2_timing-GS`, `intr_2_mode0_timing_sprites`, `hblank_ly_scx_timing-GS`, `lcdon_write_timing-GS`, `stat_lyc_onoff`. |
| 15 | Cycle counts of translated code | OK | Every emitted instruction ticks its M-cycles (`gbrt_timed_*` helpers) and places the bus access in the right M-cycle (`tests/test_codegen_bus_phase.cpp`). Blargg `instr_timing`, `mem_timing` 1/2 and Mooneye `add_sp_e_timing`, `ld_hl_sp_e_timing`, `call_timing`, `call_cc_timing`, `jp_timing`, `jp_cc_timing`, `ret_timing`, `ret_cc_timing`, `reti_timing`, `rst_timing`, `push_timing`, `pop_timing` pass. |
| 16 | Stack: PUSH/POP/CALL/RET/RST, non-C stack use | OK | SP and the stack are guest memory; pushes and pops are bus writes/reads (`gbrt_timed_stack_write16/read16`). A guest `ret` pops PC and returns to the dispatcher, so games that manipulate return addresses work. Timing tests above pass. |
| 17 | Self-modifying code | OK\* | Code in WRAM/HRAM is either interpreted (`gbrt_try_execute_ram_stub`) or, for analyzer-recognised overlays, compiled and **verified byte-for-byte against live RAM on every dispatch** before the native body runs (`c_emitter.cpp:3425-3470`). Caveat: a modification made while inside a compiled body is not noticed until the next dispatch. |
| 18 | Bank switching (MBCs) | FIXED | Mapper registers modelled for MBC1 (incl. mode 1 lower window and MBC1M), MBC2, MBC3+RTC, MBC5. Mooneye `mbc1/bits_bank1`, `bits_bank2`, `bits_mode`, `bits_ramg`, `rom_512kb`, `multicart_rom_8Mb`, `mbc5/rom_512kb`, `mbc2/bits_ramg` pass. Two fixes in this fork: (a) home-bank calls into `$4000-$7FFF` were statically bound to bank 1 (PR #21); (b) cartridge RAM was bounds-checked instead of mirrored to the chip size, so an 8 KiB MBC1 cart in mode 1 read `$FF` from "banks" 1-3 — Mooneye `mbc1/ram_64kb` failed. Now mirrored (`gb_eram_index`). |
| 19 | RST as control flow | OK | `RST n` is emitted as `gbrt_timed_rst` + call of the vector body. The `rst 0 / jp hl` jump-table idiom resolves through the dispatcher (verified in Link's Awakening). Mooneye `rst_timing` pass. |
| 20 | CALL/RET not following C conventions | OK | No C calling convention is imposed: all registers, flags and SP live in `GBContext`; a generated function is a body, not a C function with arguments. |
| 21 | Register preservation across calls | OK | Registers never live in C locals across a call; the analyzer additionally treats every register as unknown after CALL (`analyzer.cpp` "no callee-saved convention" comment), so no constant is wrongly propagated. |
| 22 | `$FF00-$FFFF` is not plain RAM | FIXED | Fixed today: unmapped registers (`$FF03`, `$FF08-$FF0E`, `$FF15`, `$FF1F`, `$FF27-$FF2F`, and `$FF4C-$FF7F` on DMG) read `$FF`, and unused bits of SC, TAC, STAT and the NRxx registers read as 1. Mooneye `bits/unused_hwio-GS` passes (it fails upstream). |
| 23 | Little-endian 16-bit values | OK | `gb_read16`/`gb_write16` low byte first (`gbrt.c:3154`); register unions are `{lo, hi}`. |
| 24 | 8/16-bit wraparound | OK | Fixed-width unsigned arithmetic in every helper; `cpu_instrs` pass. |
| 25 | Instruction side effects | OK | The IR carries every register/flag effect; `cpu_instrs` and `mem_timing` pass. |
| 26 | Instruction atomicity / boundaries | OK | One emitted group per instruction with bus phases inside it; interrupts are only serviced at boundaries. The fast-path DMA window gap (#7) is fixed. |
| 27 | F register low nibble reads as 0 | OK | `PUSH AF` masks `& 0xFFF0`, `POP AF` discards the low nibble (`c_emitter.cpp:1620-1637`). Mooneye `bits/reg_f` pass. |
| 28 | DAA | OK | `gb_daa` (gbrt.c:3598) implements the N/H/C table. Mooneye `instr/daa` and Blargg `cpu_instrs` 01 pass. |
| 29 | Rotates and shifts, with and without carry | OK | `cpu_instrs` 07-10 pass (`RLCA/RRCA/RLA/RRA` clear Z; CB rotates set Z). |
| 30 | BIT sets flags | OK | `cpu_instrs` 10-bit ops pass (Z from the bit, N=0, H=1, C preserved). |
| 31 | SWAP | OK | `cpu_instrs` 10 pass. |
| 32 | HL as implicit pointer (LDI/LDD/INC HL) | OK | `gbrt_timed_hl_read_auto`/`write_auto` combine the access and the pointer update in the right M-cycles; `mem_timing` pass. |
| 33 | Aliasing between representations | OK\* | One register file, one bus, one PPU state. Caveat: `ctx->rom_bank` is a cached copy of `gb_resolve_rom_bank` and must be kept in sync by every mapper write and by savestates (review §5). |

## Results summary (native free-running mode)

| Suite | Result |
|-------|--------|
| Blargg `cpu_instrs` | 11/11 sub-tests pass |
| Blargg `instr_timing`, `mem_timing`, `mem_timing-2`, `halt_bug` | pass |
| Blargg `oam_bug` | fails sub-tests 2, 4, 5, 7, 8 |
| Blargg `dmg_sound` | sub-test 01 (registers) fails; APU findings are in the review |
| Mooneye curated subset (59 ROMs) + 8 more PPU tests | all pass after the fixes in this fork |

## Not on the list but relevant

- The analyzer's static bank binding (item 18a) is the only defect found that
  changes gameplay in a commercial title; it is fixed on this fork and in
  upstream PR #21.
- The differential harness cannot see any free-running-only bug because it
  single-steps; the runs above are the first native-mode evidence for this
  tree on Windows. `docs/history/REVIEW-2026-09-09.md` proposes a free-running
  differential mode.
