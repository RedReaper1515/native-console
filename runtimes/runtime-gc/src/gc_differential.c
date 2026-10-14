/*
 * Two machines, compared. See gc_differential.h for what this assumes.
 */
#include "gc_differential.h"
#include "wii.h"

#include <stddef.h>
#include <string.h>

#include "gc.h"
#include "gc_efb.h"
#include "gc_gx_thread.h"

const char *gc_io_slot_name(unsigned slot, uint32_t *address);

int gc_differential_catch_up(GcMachine *recompiled, GcMachine *interpreted) {
  GcMachine *const a = recompiled, *const b = interpreted;
  while (b->cpu.cycles < a->cpu.cycles && !b->finished && !b->cpu.stopped) gc_machine_step(b);
  if (b->cpu.cycles != a->cpu.cycles) return -1;
  /* The recompiled machine may have looked at what is due before the next
   * instruction on its way out of a block - and taken an interrupt, which
   * moves its program counter without retiring anything. The interpreter
   * looks only when it steps again. Let it look now. */
  if (b->cpu.pc != a->cpu.pc && !b->cpu.stopped && !b->finished) gc_machine_before(b);
  return 0;
}

/* A piece of state, by where it is in the machine and how wide its
 * elements are, so that a difference can be named and shown. */
typedef struct GcDiffField {
  const char *name;
  size_t offset;
  size_t size;
  size_t element;
} GcDiffField;

#define CPU(field, element) {#field, offsetof(GcMachine, cpu.field), sizeof(((GcCpu *)0)->field), element}
#define MACHINE(field, element) {#field, offsetof(GcMachine, field), sizeof(((GcMachine *)0)->field), element}

static const GcDiffField cpu_fields[] = {
    CPU(pc, 4),          CPU(cycles, 8),         CPU(gpr, 4),          CPU(fpr, 16),
    CPU(cr, 4),          CPU(xer, 4),            CPU(lr, 4),           CPU(ctr, 4),
    CPU(msr, 4),         /* fpscr: below */          CPU(gqr, 4),          CPU(hid0, 4),
    CPU(hid1, 4),        CPU(hid2, 4),           CPU(wpar, 4),         CPU(dmau, 4),
    CPU(dmal, 4),        CPU(l2cr, 4),           CPU(sdr1, 4),         CPU(ear, 4),
    CPU(pvr, 4),         CPU(srr0, 4),           CPU(srr1, 4),         CPU(dar, 4),
    CPU(dsisr, 4),       CPU(sprg, 4),           CPU(sr, 4),           CPU(ibat, 4),
    CPU(dbat, 4),        CPU(reserve_addr, 4),   CPU(reserve, 1),      CPU(tb_base, 8),
    CPU(tb_base_cycles, 8), CPU(dec, 4),         CPU(dec_write_cycles, 8), CPU(stopped, 1),
    CPU(pending_vector, 4),
};

static const GcDiffField machine_fields[] = {
    MACHINE(field, 8),
    MACHINE(next_field_cycles, 8),
    MACHINE(finished, 4),
    MACHINE(exit_code, 4),
    MACHINE(pi_cause, 4),
    MACHINE(pi_mask, 4),
    MACHINE(locked_cache, 1),
    MACHINE(exi_status, 4),
    MACHINE(exi_command, 4),
    MACHINE(exi_selected, 4),
    MACHINE(aid_address, 4),
    MACHINE(aid_blocks_left, 4),
    MACHINE(aid_next_block_cycles, 8),
    MACHINE(aid_running, 4),
    MACHINE(ai_started_cycles, 8),
    MACHINE(ai_counted_before, 4),
    MACHINE(vi_line, 4),
    MACHINE(vi_column, 4),
    MACHINE(vi_pixel, 4),
    MACHINE(dsp_mail_to_cpu, 4),
    MACHINE(dsp_mail_to_dsp, 4),
    MACHINE(dsp_mail_waiting, 4),
    MACHINE(dsp_boot_parameter, 4),
    MACHINE(dsp_ucode_address, 4),
    MACHINE(dsp_ucode_length, 4),
    MACHINE(dsp_mail_queued, 4),
    MACHINE(dsp_mail_due_cycles, 8),
    MACHINE(dsp_list_length, 4),
    MACHINE(dsp_control_previous, 4),
    MACHINE(dsp_init_code_clears, 8),
    MACHINE(di_status, 4),
    MACHINE(di_cover, 4),
    MACHINE(disc_error, 4),
    MACHINE(unknown_disc_commands, 4),
    MACHINE(gather, 1),
    MACHINE(gather_used, 4),
    MACHINE(gather_bursts, 4),
    MACHINE(pe_control, 4),
    MACHINE(unknown_io_reads, 4),
    MACHINE(unknown_io_writes, 4),
    MACHINE(bad_access, 4),
    /* The graphics processor's register files and ring, and what it has been
     * asked to draw. */
    MACHINE(gx.fifo_base, 4),
    MACHINE(gx.fifo_end, 4),
    MACHINE(gx.read_ptr, 4),
    MACHINE(gx.write_ptr, 4),
    MACHINE(gx.linked, 4),
    MACHINE(gx.bp, 4),
    MACHINE(gx.xf, 4),
    MACHINE(gx.xf_memory, 4),
    MACHINE(gx.matrix_index_a, 4),
    MACHINE(gx.matrix_index_b, 4),
    MACHINE(gx.vcd_lo, 4),
    MACHINE(gx.vcd_hi, 4),
    MACHINE(gx.vat_a, 4),
    MACHINE(gx.vat_b, 4),
    MACHINE(gx.vat_c, 4),
    MACHINE(gx.array_base, 4),
    MACHINE(gx.array_stride, 4),
    MACHINE(gx.commands, 8),
    MACHINE(gx.primitives, 8),
    MACHINE(gx.vertices, 8),
    MACHINE(gx.triangles, 8),
    MACHINE(gx.display_lists, 8),
    MACHINE(gx.bytes, 8),
    MACHINE(efb.copies, 8),
    MACHINE(efb.display_address, 4),
};

static uint64_t element_value(const uint8_t *p, size_t element) {
  switch (element) {
    case 1: return *p;
    case 4: {
      uint32_t v;
      memcpy(&v, p, 4);
      return v;
    }
    default: {
      uint64_t v;
      memcpy(&v, p, 8);
      return v;
    }
  }
}

static void print_element(FILE *out, const uint8_t *p, size_t element) {
  if (element == 16) {
    fprintf(out, "%016llx/%016llx", (unsigned long long)element_value(p, 8),
            (unsigned long long)element_value(p + 8, 8));
  } else if (element == 8) {
    fprintf(out, "%016llx", (unsigned long long)element_value(p, 8));
  } else if (element == 4) {
    fprintf(out, "%08llx", (unsigned long long)element_value(p, 4));
  } else {
    fprintf(out, "%02llx", (unsigned long long)element_value(p, 1));
  }
}

static unsigned compare_fields(const GcMachine *a, const GcMachine *b, const GcDiffField *fields,
                               size_t count, FILE *out) {
  unsigned differ = 0;
  size_t f;
  for (f = 0; f < count; f++) {
    const uint8_t *pa = (const uint8_t *)a + fields[f].offset;
    const uint8_t *pb = (const uint8_t *)b + fields[f].offset;
    const size_t elements = fields[f].size / fields[f].element;
    size_t e, shown = 0;
    if (!memcmp(pa, pb, fields[f].size)) continue;
    differ++;
    if (!out) continue;
    for (e = 0; e < elements && shown < 6u; e++) {
      const size_t at = e * fields[f].element;
      if (!memcmp(pa + at, pb + at, fields[f].element)) continue;
      if (elements > 1) fprintf(out, "  %s[%u]: recompiled ", fields[f].name, (unsigned)e);
      else fprintf(out, "  %s: recompiled ", fields[f].name);
      print_element(out, pa + at, fields[f].element);
      fputs(", interpreted ", out);
      print_element(out, pb + at, fields[f].element);
      fputs("\n", out);
      shown++;
    }
  }
  return differ;
}

unsigned gc_differential_compare_cpu(const GcMachine *recompiled, const GcMachine *interpreted, FILE *out) {
  unsigned differ =
      compare_fields(recompiled, interpreted, cpu_fields, sizeof(cpu_fields) / sizeof(cpu_fields[0]), out);
  /* FPSCR as the guest would read it: the register can be a note behind
   * (gc_fpu_impl.h), and the two machines are equal when what they would
   * show is. */
  const uint32_t fa = gc_cpu_fpscr(&recompiled->cpu), fb = gc_cpu_fpscr(&interpreted->cpu);
  if (fa != fb) {
    differ++;
    if (out) fprintf(out, "  fpscr: recompiled %08x, interpreted %08x\n", fa, fb);
  }
  return differ;
}

/* A block of memory: how many bytes differ, and the first few runs of them
 * with what each machine holds there. */
static unsigned compare_bytes(const char *what, uint32_t shown_base, const uint8_t *a, const uint8_t *b,
                              size_t size, FILE *out) {
  size_t i, bytes = 0, runs = 0;
  if (!memcmp(a, b, size)) return 0;
  if (!out) return 1;
  for (i = 0; i < size;) {
    size_t start, k;
    if (a[i] == b[i]) {
      /* Skip equal stretches a word at a time. */
      if (i + 8u <= size && !memcmp(a + i, b + i, 8u)) i += 8u;
      else i++;
      continue;
    }
    start = i;
    while (i < size && a[i] != b[i]) i++;
    bytes += i - start;
    if (runs++ < 8u) {
      fprintf(out, "  %s %08x, %u byte%s: recompiled", what, shown_base + (uint32_t)start,
              (unsigned)(i - start), i - start == 1 ? "" : "s");
      for (k = start; k < i && k < start + 8u; k++) fprintf(out, " %02x", a[k]);
      fputs(", interpreted", out);
      for (k = start; k < i && k < start + 8u; k++) fprintf(out, " %02x", b[k]);
      fputs("\n", out);
    }
  }
  fprintf(out, "  %s: %u bytes differ in %u runs\n", what, (unsigned)bytes, (unsigned)runs);
  return 1;
}

static unsigned compare_plane(const char *what, const uint32_t *a, const uint32_t *b, FILE *out) {
  const size_t pixels = (size_t)GC_EFB_WIDTH * GC_EFB_HEIGHT;
  size_t i, differ = 0, first = 0;
  if (!memcmp(a, b, pixels * sizeof(uint32_t))) return 0;
  if (!out) return 1;
  for (i = 0; i < pixels; i++) {
    if (a[i] == b[i]) continue;
    if (!differ) first = i;
    differ++;
  }
  fprintf(out, "  efb %s: %u pixels differ, the first at (%u, %u): recompiled %08x, interpreted %08x\n", what,
          (unsigned)differ, (unsigned)(first % GC_EFB_WIDTH), (unsigned)(first / GC_EFB_WIDTH), a[first],
          b[first]);
  return 1;
}

unsigned gc_differential_compare(GcMachine *recompiled, GcMachine *interpreted, FILE *out) {
  GcMachine *const a = recompiled, *const b = interpreted;
  unsigned differ = 0, slot;
  /* What a graphics thread has still to draw, drawn. */
  gc_gx_thread_sync(a);
  gc_gx_thread_sync(b);
  gc_machine_sync(a);
  gc_machine_sync(b);
  differ += gc_differential_compare_cpu(a, b, out);
  differ += compare_fields(a, b, machine_fields, sizeof(machine_fields) / sizeof(machine_fields[0]), out);
  for (slot = 0; slot < GC_IO_STORAGE; slot++) {
    uint32_t address = 0;
    const char *name;
    if (a->io[slot] == b->io[slot]) continue;
    differ++;
    if (!out) continue;
    name = gc_io_slot_name(slot, &address);
    fprintf(out, "  I/O %s (%08x): recompiled %08x, interpreted %08x\n", name ? name : "(unused slot)", address,
            a->io[slot], b->io[slot]);
  }
  /* A Wii's own state: Hollywood, the mailbox, and everything IOS and its
   * devices hold (wii.h). Compared as bytes, and reported by where the first
   * difference is - an offset into WiiState is enough to name the field. */
  if (a->cpu.machine && b->cpu.machine) {
    const WiiState *const wa = (const WiiState *)a->cpu.machine;
    const WiiState *const wb = (const WiiState *)b->cpu.machine;
    const size_t size = offsetof(WiiState, host);
    if (memcmp(wa, wb, size) != 0) {
      const uint8_t *const pa = (const uint8_t *)wa, *const pb = (const uint8_t *)wb;
      size_t at = 0;
      while (at < size && pa[at] == pb[at]) at++;
      differ++;
      if (out) {
        fprintf(out, "  Wii state differs first at byte %u of %u (recompiled %02x, interpreted %02x)\n",
                (unsigned)at, (unsigned)size, pa[at], pb[at]);
      }
    }
  }
  /* Reported from MEM1's base; on a Wii, bytes past 24 MB are MEM2's. */
  differ += compare_bytes("main memory", 0x80000000u, a->mem1, b->mem1, gc_ram_size, out);
  differ += compare_bytes("ARAM", 0x00000000u, a->aram, b->aram, GC_ARAM_SIZE, out);
  differ += compare_plane("colour", a->efb.color, b->efb.color, out);
  differ += compare_plane("depth", a->efb.depth, b->efb.depth, out);
  return differ;
}
