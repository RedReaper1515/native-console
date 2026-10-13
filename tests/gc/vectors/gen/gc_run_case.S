/*
 * Run one instruction with a known register state and capture what it left.
 *
 * The whole register file is saved before the case and restored after, so a
 * case may legitimately write r0, r1, r2 or any of r13-r31 - which lmw,
 * stmw and the string forms do - without destroying the C runtime that
 * called us. Nothing between the case and the capture touches the stack,
 * because the case may have just overwritten the stack pointer.
 *
 * Register roles:
 *   r3-r10  the case's inputs and outputs (r10 points at the scratch buffer)
 *   f1-f8   the case's floating-point inputs and outputs, both lanes
 *   f0, r11 the harness's own scratch, never encoded into a case
 */

#include "gc_vector_state.h"

	.section .text
	.globl gc_run_case
	.type gc_run_case, @function
	.align 2

gc_run_case:
	/* r3 = state. Save every register plus LR, CR and XER. */
	lis	11, gc_saved@ha
	addi	11, 11, gc_saved@l
	stmw	0, 0(11)		/* r0..r31 -> gc_saved[0..31] */
	mflr	0
	stw	0, 128(11)
	mfcr	0
	stw	0, 132(11)
	mfxer	0
	stw	0, 136(11)
	mffs	0
	stfd	0, 144(11)
	stfd	1, 152(11)
	stfd	2, 160(11)
	stfd	3, 168(11)
	stfd	4, 176(11)
	stfd	5, 184(11)
	stfd	6, 192(11)
	stfd	7, 200(11)
	stfd	8, 208(11)

	/* The state pointer has to survive a case that rewrites the whole
	 * register file, so it lives in memory, not a register. */
	stw	3, 140(11)

	mr	11, 3

	/* Floating point first: setting both lanes needs f0 as scratch, and
	 * ps_merge00 frD,frA,frB gives (frA.ps0, frB.ps0). */
	lfd	1, GC_ST_FPR+0(11)
	lfd	0, GC_ST_FPR+8(11)
	ps_merge00 1, 1, 0
	lfd	2, GC_ST_FPR+16(11)
	lfd	0, GC_ST_FPR+24(11)
	ps_merge00 2, 2, 0
	lfd	3, GC_ST_FPR+32(11)
	lfd	0, GC_ST_FPR+40(11)
	ps_merge00 3, 3, 0
	lfd	4, GC_ST_FPR+48(11)
	lfd	0, GC_ST_FPR+56(11)
	ps_merge00 4, 4, 0
	lfd	5, GC_ST_FPR+64(11)
	lfd	0, GC_ST_FPR+72(11)
	ps_merge00 5, 5, 0
	lfd	6, GC_ST_FPR+80(11)
	lfd	0, GC_ST_FPR+88(11)
	ps_merge00 6, 6, 0
	lfd	7, GC_ST_FPR+96(11)
	lfd	0, GC_ST_FPR+104(11)
	ps_merge00 7, 7, 0
	lfd	8, GC_ST_FPR+112(11)
	lfd	0, GC_ST_FPR+120(11)
	ps_merge00 8, 8, 0

	/* FPSCR, CR and XER. mtfsf takes the low word of an FPR. */
	lfd	0, GC_ST_FPSCR(11)
	mtfsf	255, 0
	lwz	0, GC_ST_CR(11)
	mtcr	0
	lwz	0, GC_ST_XER(11)
	mtxer	0

	/* The visible GPRs last: r10 is overwritten with the scratch
	 * address so that a memory form has somewhere legal to point. */
	lwz	3, GC_ST_GPR+0(11)
	lwz	4, GC_ST_GPR+4(11)
	lwz	5, GC_ST_GPR+8(11)
	lwz	6, GC_ST_GPR+12(11)
	lwz	7, GC_ST_GPR+16(11)
	lwz	8, GC_ST_GPR+20(11)
	lwz	9, GC_ST_GPR+24(11)
	addi	10, 11, GC_ST_MEM

	bl	gc_case_code

	/* Capture. The stack pointer may be rubbish now, so everything goes
	 * through an absolute address and r11. */
	lis	11, gc_case_out@ha
	addi	11, 11, gc_case_out@l
	stw	3, GC_ST_GPR+0(11)
	stw	4, GC_ST_GPR+4(11)
	stw	5, GC_ST_GPR+8(11)
	stw	6, GC_ST_GPR+12(11)
	stw	7, GC_ST_GPR+16(11)
	stw	8, GC_ST_GPR+20(11)
	stw	9, GC_ST_GPR+24(11)
	stw	10, GC_ST_GPR+28(11)
	mfcr	3
	stw	3, GC_ST_CR(11)
	mfxer	3
	stw	3, GC_ST_XER(11)
	mffs	0
	stfd	0, GC_ST_FPSCR(11)

	stfd	1, GC_ST_FPR+0(11)
	ps_merge11 0, 1, 1
	stfd	0, GC_ST_FPR+8(11)
	stfd	2, GC_ST_FPR+16(11)
	ps_merge11 0, 2, 2
	stfd	0, GC_ST_FPR+24(11)
	stfd	3, GC_ST_FPR+32(11)
	ps_merge11 0, 3, 3
	stfd	0, GC_ST_FPR+40(11)
	stfd	4, GC_ST_FPR+48(11)
	ps_merge11 0, 4, 4
	stfd	0, GC_ST_FPR+56(11)
	stfd	5, GC_ST_FPR+64(11)
	ps_merge11 0, 5, 5
	stfd	0, GC_ST_FPR+72(11)
	stfd	6, GC_ST_FPR+80(11)
	ps_merge11 0, 6, 6
	stfd	0, GC_ST_FPR+88(11)
	stfd	7, GC_ST_FPR+96(11)
	ps_merge11 0, 7, 7
	stfd	0, GC_ST_FPR+104(11)
	stfd	8, GC_ST_FPR+112(11)
	ps_merge11 0, 8, 8
	stfd	0, GC_ST_FPR+120(11)

	/* Put the machine back exactly as it was. */
	lis	11, gc_saved@ha
	addi	11, 11, gc_saved@l
	lfd	0, 144(11)
	mtfsf	255, 0
	lfd	1, 152(11)
	lfd	2, 160(11)
	lfd	3, 168(11)
	lfd	4, 176(11)
	lfd	5, 184(11)
	lfd	6, 192(11)
	lfd	7, 200(11)
	lfd	8, 208(11)
	lwz	0, 128(11)
	mtlr	0
	lwz	0, 132(11)
	mtcr	0
	lwz	0, 136(11)
	mtxer	0
	/* lmw must not load its own base register, so r0-r11 come back one at
	 * a time and r11, the base, comes last. */
	lmw	12, 48(11)
	lwz	0, 0(11)
	lwz	1, 4(11)
	lwz	2, 8(11)
	lwz	3, 12(11)
	lwz	4, 16(11)
	lwz	5, 20(11)
	lwz	6, 24(11)
	lwz	7, 28(11)
	lwz	8, 32(11)
	lwz	9, 36(11)
	lwz	10, 40(11)
	lwz	11, 44(11)
	blr
	.size gc_run_case, .-gc_run_case

	.section .bss
	.globl gc_saved
	.align 5
gc_saved:
	.space 216

	.globl gc_case_code
	.align 5
gc_case_code:
	.space 32

	.globl gc_case_out
	.align 5
gc_case_out:
	.space GC_ST_SIZE
