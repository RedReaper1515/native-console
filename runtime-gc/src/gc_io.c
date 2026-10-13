/*
 * The I/O register file.
 *
 * Every block of hardware registers is described by a table - offset,
 * width, name, which bits read back, which bits the guest may change, and a
 * hook where behaviour is needed. The default is storage: a register the
 * guest writes and reads back. That is enough for a surprising amount of
 * console software, and where it is not enough the hook says so explicitly
 * instead of being buried in a switch.
 *
 * An access to an offset no table describes still works, so a program does
 * not die on a register nobody has got to yet, but it is counted and the
 * first few are logged. The count is part of a run's summary: "this program
 * finished" means much less if it spent the whole time writing to registers
 * that do nothing.
 */

#include <string.h>

#include "gc.h"
#include "gc_zelda.h"
#include "gc_ax.h"
#include "gc_audio.h"
#include "gc_card.h"
#include "gc_disc.h"
#include "gc_gx.h"
#include "gc_memory.h"

/* ------------------------------------------------------------------ */
/* Hooks                                                               */

/* Writing the interrupt cause register acknowledges the bits written. */
static void pi_cause_write(GcMachine *m, uint32_t value) { m->pi_cause &= ~value; }
static uint32_t pi_cause_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return m->pi_cause;
}

static void pi_mask_write(GcMachine *m, uint32_t value) { m->pi_mask = value; }
static uint32_t pi_mask_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return m->pi_mask;
}

/* The console type a program reads to tell a retail machine from a
 * development one. This is a retail GameCube. */
static uint32_t pi_console_read(GcMachine *m, uint32_t stored) {
  (void)m;
  (void)stored;
  return 0x00000023u;
}

/* Where the beam is, which software polls to wait for a retrace or for a
 * particular line. Two halfwords: the vertical position at +0x2c and the
 * horizontal at +0x2e, both counting from one. libogc reads the vertical
 * one twice around the horizontal read and retries if it moved, so a
 * machine whose beam does not move makes it spin instead - and
 * VIDEO_GetCurrentLine returns a constant, which is how this was found
 * (tests/gc/programs/gc_timetest.c). */
static uint32_t vi_position_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return ((m->vi_line + 1u) << 16) | (m->vi_column + 1u);
}

/* ------------------------------------------------------------------ */
/* The DSP, as far as booting needs it                                 */

/* The SDK resets the DSP, waits for its ROM to announce itself through the
 * mailbox, then uploads microcode over ARAM DMA. None of that microcode
 * runs here - the audio DSP is P5 - but the handshake has to happen or the
 * SDK never finishes starting up, and every hwtests program is an SDK
 * program.
 *
 * The value the ROM announces itself with, 0x8071feed, is what the hardware
 * sends; it is documented by Dolphin's DSP-HLE and by DSPSpy's dumps of the
 * real ROM. */
#define GC_DSP_ROM_HELLO 0x8071feedu

/* DSP control register bits. */
#define GC_DSP_RESET 0x0001u
#define GC_DSP_INT 0x0002u
#define GC_DSP_HALT 0x0004u
#define GC_DSP_AUDIO_INT 0x0008u
#define GC_DSP_ARAM_INT 0x0020u
#define GC_DSP_DSP_INT 0x0080u
#define GC_DSP_ARAM_DMA 0x0200u
#define GC_DSP_INIT_CODE 0x0400u
#define GC_DSP_INIT 0x0800u

static uint32_t *io_slot(GcMachine *m, const char *block_name, uint32_t offset);
static void dsp_update_interrupt(GcMachine *m);

/* The hot registers, found once. See the note in gc.h. */
#define GC_SLOT_MISSING 0xffffffffu

static void resolve_hot_slots(GcMachine *m) {
  uint32_t *slot;
  if (m->hot_slots_known) return;
  slot = io_slot(m, "ai", 0x00);
  m->ai_control_slot = slot ? (uint32_t)(slot - m->io) : GC_SLOT_MISSING;
  slot = io_slot(m, "ai", 0x0c);
  m->ai_timing_slot = slot ? (uint32_t)(slot - m->io) : GC_SLOT_MISSING;
  slot = io_slot(m, "dsp", 0x0a);
  m->dsp_control_slot = slot ? (uint32_t)(slot - m->io) : GC_SLOT_MISSING;
  m->hot_slots_known = 1;
}

static uint32_t *hot_slot(GcMachine *m, uint32_t index) {
  return index == GC_SLOT_MISSING ? NULL : &m->io[index];
}



static void dsp_send_to_cpu(GcMachine *m, uint32_t mail) {
  m->dsp_mail_to_cpu = mail;
  m->dsp_mail_waiting = 1;
}

/* Mail from the CPU.
 *
 * The ROM speaks a small upload protocol: a word of the form 0x80f3xxxx
 * names a parameter and the word after it is that parameter's value. When
 * the start address arrives the ROM jumps into whatever was uploaded, and
 * the new microcode announces itself with a word of its own. Anything the
 * ROM does not recognise it echoes back.
 *
 * The microcode the SDK uploads during start-up is its "init" one, and the
 * value it announces itself with is what this sends. What a game uploads
 * afterwards is a task microcode - AX, for sound - which says something
 * different, and the two are told apart below.
 *
 * The protocol and its constants are the hardware's, documented by
 * Dolphin's DSP-HLE and by DSPSpy's dumps of the real ROM. */
#define GC_DSP_INIT_HELLO 0x80544348u

/* What a *task* microcode says, and what the driver says back.
 *
 * A game does not talk to the ROM. It uploads a microcode - AX, for sound
 * - and from then on the conversation is between that microcode and the
 * SDK's DSP driver, in five messages. Only two of them matter for getting
 * a game running: the task has started, and the task is offering to stop.
 * The driver acknowledges the second and hands over the next piece of
 * work, and that loop is the whole of a game's audio frame.
 *
 * docs/gc-dsp-ax.md, part A, has all five and where each comes from. */
#define GC_DSP_TASK_INIT 0xdcd10000u    /* the task has started */
#define GC_DSP_TASK_YIELD 0xdcd10002u   /* the task is offering to stop */
#define GC_DSP_ACK_CONTINUE 0xcdd10003u /* carry on */
#define GC_DSP_ACK_SWITCH 0xcdd10001u   /* stop; a context block follows */
/* A command list, handed over as a length and then an address. */
#define GC_DSP_LIST_LENGTH 0xbabe0000u

/* How long the DSP takes to notice.
 *
 * Nothing here is doing any work, so every answer could be instant - and
 * an instant answer is precisely what breaks games. At least one
 * commercial title keeps its own count of how long a command list should
 * take and hangs at boot if the reply arrives sooner than it believes
 * possible. The number below is a floor rather than a measurement: it is
 * far above the smallest that is known to avoid the hang and far below
 * anything a game would notice as slow. **Unverified:** what the hardware
 * really takes, which depends on the list. */
#define GC_DSP_REPLY_DELAY 20000u
/* ... and how long between two words it sends one after the other: a few of
 * its own instructions. **Unverified** as a number; what matters is that it
 * is not a reply's worth. */
#define GC_DSP_FOLLOW_DELAY 64u

/* Mail the DSP will send, once enough time has passed. */
static void dsp_queue_reply(GcMachine *m, uint32_t mail) {
  m->dsp_mail_queued = mail;
  m->dsp_mail_due_cycles = m->cpu.cycles + GC_DSP_REPLY_DELAY;
}

/* The Zelda family's mail, one at a time (gc_zelda.h): the oldest it owes
 * becomes the mail the machine is holding back, and is delivered like any
 * other when its time comes. */
static void dsp_stage_zelda_mail(GcMachine *m) {
  uint32_t mail;
  int interrupts;
  if (m->dsp_mail_queued || !gc_zelda_next_mail(m, &mail, &interrupts)) return;
  gc_ax_zelda(m)->staged_interrupts = (uint32_t)interrupts;
  dsp_queue_reply(m, mail);
  /* A word that does not interrupt is the second of a pair: the microcode
   * writes it as soon as the first has been taken, and the game's handler
   * sits reading the mailbox until it is there. Made to wait as long as a
   * reply, that was nineteen thousand register reads a field in Super Mario
   * Galaxy 2 - the handler spinning for a delay nothing on the console
   * imposes. */
  if (!interrupts) m->dsp_mail_due_cycles = m->cpu.cycles + GC_DSP_FOLLOW_DELAY;
}

static void dsp_boot_ucode(GcMachine *m) {
  if (m->trace_io && m->log) {
    fprintf(m->log, "gc: the DSP booted microcode at %08x (%u bytes)\n", m->dsp_ucode_address,
            m->dsp_ucode_length);
  }
  /* Which sound microcode. There are two families and they share nothing
   * but the mailbox, so the bytes the game uploaded are looked at: a member
   * of the Zelda family is recognised by its identification, and anything
   * else is taken to be AX, as everything was before the other existed. */
  if (m->dsp_ucode_address && m->dsp_ucode_length) {
    const uint8_t *code = gc_ram_at_register(m, m->dsp_ucode_address, m->dsp_ucode_length);
    const uint32_t hash = code ? gc_dsp_ucode_hash(code, m->dsp_ucode_length) : 0u;
    uint32_t flags = 0;
    if (code && gc_zelda_knows(hash, &flags)) {
      if (gc_zelda_boot(m, hash) == 0) {
        if (m->log) {
          fprintf(m->log, "gc: the sound microcode is %08x, of the Zelda family\n", hash);
        }
        dsp_stage_zelda_mail(m);
        return;
      }
      if (m->log) {
        fprintf(m->log, "gc: the sound microcode is %08x, of the Zelda family's early members, "
                        "whose protocol is not written; the game is answered as if it were AX "
                        "and will wait for ever\n", hash);
      }
    } else if (m->trace_io && m->log) {
      fprintf(m->log, "gc: the sound microcode is %08x, taken to be AX\n", hash);
    }
  }
  /* Which microcode, and therefore which greeting.
   *
   * The ROM's own init path is booted with no address and no length - that
   * is what libogc's start-up does, and what it waits for is the init
   * microcode's word. A game uploads a real microcode to a real address,
   * and what *that* announces is that a task has started. The two are
   * different conversations and the difference is visible right here,
   * which is better than guessing from a size. */
  if (m->dsp_ucode_address) {
    dsp_queue_reply(m, GC_DSP_TASK_INIT);
  } else {
    dsp_send_to_cpu(m, GC_DSP_INIT_HELLO);
  }
}

static void dsp_receive(GcMachine *m, uint32_t mail) {
  m->dsp_mail_to_dsp = mail;

  /* The Zelda family's microcode has the mailbox to itself while it runs. */
  if (gc_zelda_active(m)) {
    switch (gc_zelda_mail(m, mail)) {
      case GC_ZELDA_MAIL_TO_ROM:
        /* Back to the ROM, which says what it said out of reset. */
        m->dsp_boot_parameter = 0;
        dsp_send_to_cpu(m, GC_DSP_ROM_HELLO);
        return;
      case GC_ZELDA_MAIL_NEW_UCODE: {
        /* Another microcode for a while - on a GameCube, the one that
         * unlocks a memory card. Only coming back to the same one is
         * written: it is told it has resumed. Anything else is started as
         * an upload through the ROM would have started it, and said so. */
        uint32_t address, length;
        const uint8_t *code;
        gc_zelda_handover(m, &address, &length);
        code = length ? gc_ram_at_register(m, address, length) : NULL;
        if (code && gc_dsp_ucode_hash(code, length) == gc_ax_zelda(m)->hash) {
          gc_zelda_resume(m);
        } else {
          if (m->log) {
            fprintf(m->log, "gc: the sound microcode handed over to another at %08x (%u bytes, "
                            "%08x), which is started from nothing\n", address, length,
                    code ? gc_dsp_ucode_hash(code, length) : 0u);
          }
          gc_ax_zelda(m)->active = 0;
          m->dsp_ucode_address = address;
          m->dsp_ucode_length = length;
          dsp_boot_ucode(m);
          return;
        }
        break;
      }
      default: break;
    }
    dsp_stage_zelda_mail(m);
    return;
  }

  /* A command list: its length, and then on the next message its address.
   * Nothing is done with it yet - decoding the list is what turns this
   * into sound - but the task has to answer, and answering is what lets a
   * game get on with its frame. */
  if (m->dsp_list_length) {
    const uint32_t halfwords = m->dsp_list_length;
    m->dsp_list_length = 0;
    if (m->trace_io && m->log) {
      fprintf(m->log, "gc: a command list at %08x, %u halfwords\n", mail, halfwords);
    }
    /* The list is executed now and answered later. Now, because the game has
     * flushed it out of its cache and will reuse the memory; later, because
     * a reply that arrives in the same instruction hangs games that keep
     * their own idea of how long a list takes. */
    gc_ax_run_list(m, mail, halfwords);
    dsp_queue_reply(m, GC_DSP_TASK_YIELD);
    return;
  }
  if ((mail & 0xffff0000u) == GC_DSP_LIST_LENGTH) {
    m->dsp_list_length = (mail & 0xffffu) ? (mail & 0xffffu) : 1u;
    return;
  }
  /* The driver's answer to a yield. Nothing to do: what follows is the
   * next command list, and a switch brings ten words of context after it
   * which are equally nothing to a machine that runs one task. */
  if (mail == GC_DSP_ACK_CONTINUE || mail == GC_DSP_ACK_SWITCH) return;

  if (m->dsp_boot_parameter == 0) {
    if ((mail & 0xffff0000u) == 0x80f30000u) {
      m->dsp_boot_parameter = mail;
    } else {
      dsp_send_to_cpu(m, 0xfeee0000u | (mail & 0xffffu));
    }
    return;
  }
  switch (m->dsp_boot_parameter) {
    case 0x80f3a001u: m->dsp_ucode_address = mail; break;
    case 0x80f3a002u: m->dsp_ucode_length = mail & 0xffffu; break;
    case 0x80f3d001u:
      m->dsp_boot_parameter = 0;
      dsp_boot_ucode(m);
      return;
    default: break;
  }
  m->dsp_boot_parameter = 0;
}

static uint32_t dsp_mail_in_hi_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return (m->dsp_mail_waiting ? 0x8000u : 0u) | ((m->dsp_mail_to_cpu >> 16) & 0x7fffu);
}

static uint32_t dsp_mail_in_lo_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  /* Reading the low half takes the mail. */
  m->dsp_mail_waiting = 0;
  return m->dsp_mail_to_cpu & 0xffffu;
}

static void dsp_mail_out_lo_write(GcMachine *m, uint32_t value) {
  const uint32_t *high = io_slot(m, "dsp", 0x00);
  dsp_receive(m, ((high ? *high : 0u) << 16) | (value & 0xffffu));
}

/* The ARAM DMA the SDK uploads microcode through, and that a game uses to
 * page sound data in and out. It completes at once: nothing can observe it
 * in progress, because the busy bit reads as clear. */
/* The ARAM DMA the SDK sizes audio memory with and a game pages sound data
 * through. Each of its three parameters is a pair of 16-bit registers, and
 * writing the low half of the count is what starts the transfer - so that
 * is where the hook goes. It completes at once: nothing can observe it in
 * progress, because the busy bit reads as clear.
 */
static uint32_t io_pair(GcMachine *m, uint32_t high_offset) {
  const uint32_t *high = io_slot(m, "dsp", high_offset);
  const uint32_t *low = io_slot(m, "dsp", high_offset + 2);
  if (!high || !low) return 0;
  return ((*high & 0xffffu) << 16) | (*low & 0xffffu);
}

static void aram_dma_start(GcMachine *m, uint32_t value) {
  uint32_t *control = io_slot(m, "dsp", 0x0a);
  const uint32_t main_address = gc_ram_from_register(io_pair(m, 0x20)) & ~1u;
  const uint32_t aram_address = io_pair(m, 0x24) & 0x03fffffeu;
  const uint32_t count = (io_pair(m, 0x28) & 0xffff0000u) | (value & 0xffffu);
  const uint32_t length = count & 0x03fffffeu;
  const int to_aram = (count & 0x80000000u) == 0;

  if (length) {
    uint8_t *memory = gc_ram_ptr(m, main_address, length);
    if (memory) {
      /* Where a transfer lands. The address decodes to 64 MB, and repeats
       * after that. The first 16 MB are the audio memory; the rest is the
       * expansion port, and with nothing plugged into it a read there
       * comes back as zeroes and a write goes nowhere.
       *
       * This used to wrap at 16 MB instead, on the reasoning that the SDK
       * finds the size by aliasing. It does probe past 16 MB, and a wrap
       * answers the probe with memory that holds what was written - so the
       * SDK decided there was an expansion, handed out audio memory above
       * 16 MB, and Shrek 2's virtual memory paged into it: every page it
       * wrote out landed on top of the sound data at the bottom, and the
       * game crashed on the pointers it read back once a new game started
       * loading (docs/GC-PLAN.md, P6). With the expansion port empty the
       * probe finds 16 MB, which is what a retail console has. */
      uint32_t moved = 0;
      while (moved < length) {
        const uint32_t at = (aram_address + moved) & 0x03ffffffu;
        uint32_t piece;
        if (at < GC_ARAM_SIZE) {
          piece = GC_ARAM_SIZE - at;
          if (piece > length - moved) piece = length - moved;
          if (to_aram) {
            memcpy(m->aram + at, memory + moved, piece);
          } else {
            memcpy(memory + moved, m->aram + at, piece);
          }
        } else {
          /* The empty expansion port, up to where the decode repeats. */
          piece = 0x04000000u - at;
          if (piece > length - moved) piece = length - moved;
          if (!to_aram) memset(memory + moved, 0, piece);
        }
        moved += piece;
      }
    } else {
      m->bad_access++;
      if (m->log) {
        fprintf(m->log, "gc: an ARAM transfer from main memory that is not there: %08x\n",
                main_address);
      }
    }
  }
  if (control) *control |= GC_DSP_ARAM_INT;
  dsp_update_interrupt(m);
}

/* An interrupt line is a level, not an event: the processor interface sees
 * whatever the device is asserting. Modelling it as a pulse means the
 * handler acknowledges the device, the cause bit stays set anyway, and the
 * machine spends the rest of its life in the interrupt handler. */
static void dsp_update_interrupt(GcMachine *m) {
  const uint32_t *control;
  resolve_hot_slots(m);
  control = hot_slot(m, m->dsp_control_slot);
  const uint32_t c = control ? *control : 0u;
  const int asserted = ((c & 0x0008u) && (c & 0x0010u)) || ((c & 0x0020u) && (c & 0x0040u)) ||
                       ((c & 0x0080u) && (c & 0x0100u));
  gc_set_interrupt(m, GC_INT_DSP, asserted);
}

static uint32_t dsp_control_read(GcMachine *m, uint32_t stored) {
  /* The reset bit is self-clearing and the DMA is never in progress. The
   * bit that says the init microcode is running clears a little after it
   * starts, which is what the SDK waits on. */
  uint32_t value = stored & ~(GC_DSP_RESET | GC_DSP_ARAM_DMA);
  if ((value & GC_DSP_INIT_CODE) && m->cpu.cycles >= m->dsp_init_code_clears) {
    uint32_t *slot = io_slot(m, "dsp", 0x0a);
    if (slot) *slot &= ~GC_DSP_INIT_CODE;
    value &= ~GC_DSP_INIT_CODE;
  }
  return value;
}

/* EXPERIMENT: bit 0 of the mode register, which a game polls and nothing
 * writes. */
static uint32_t aram_mode_read(GcMachine *m, uint32_t stored) {
  (void)m;
  return stored | 1u;
}

/* The mailbox going to the DSP, read back.
 *
 * The top bit says a message is waiting to be collected; the DSP clears it
 * when it takes one, and software writes a message and then waits for that
 * bit to fall before writing another. There is no DSP here - the audio
 * work is done for it rather than by it - so the message is taken the
 * instant it is written, and the bit is never set when read.
 *
 * Leaving it set is not a missing feature that degrades the sound. It is a
 * game waiting for ever for an answer from something that was never going
 * to answer, with nothing in the log to say so. */
static uint32_t dsp_mail_out_hi_read(GcMachine *m, uint32_t stored) {
  (void)m;
  return stored & 0x7fffu;
}

static void dsp_control_write(GcMachine *m, uint32_t value) {
  uint32_t *control = io_slot(m, "dsp", 0x0a);
  const uint32_t previous = m->dsp_control_previous;
  if (!control) return;
  /* The three interrupt status bits are written back to clear. */
  *control &= ~(value & (GC_DSP_AUDIO_INT | GC_DSP_ARAM_INT | GC_DSP_DSP_INT));
  *control &= ~GC_DSP_RESET;
  if (value & GC_DSP_RESET) {
    /* Out of reset the ROM runs and announces itself. Whatever microcode
     * was running is not, and what it owed is not owed. */
    if (gc_zelda_active(m)) {
      GcZelda *zelda = gc_ax_zelda(m);
      zelda->active = 0;
      zelda->queue_count = 0;
      m->dsp_mail_queued = 0;
    }
    m->dsp_mail_waiting = 0;
    m->dsp_boot_parameter = 0;
    dsp_send_to_cpu(m, GC_DSP_ROM_HELLO);
  }
  /* The SDK starts the init microcode by *clearing* this bit, not by
   * setting it: the ROM loads the microcode on the falling edge, and
   * answers that it is running until a moment later. */
  if ((previous & GC_DSP_INIT) && !(value & GC_DSP_INIT)) {
    *control |= GC_DSP_INIT_CODE;
    m->dsp_init_code_clears = m->cpu.cycles + 130ull * GC_CYCLES_PER_TB;
    dsp_boot_ucode(m);
  }
  m->dsp_control_previous = value;
  dsp_update_interrupt(m);
}

/* ------------------------------------------------------------------ */
/* The audio interface's clock                                         */

/* The sample counter counts samples, at the rate the control register
 * picks, for as long as the control register says to. It is not part of
 * making a sound: a game starts it, asks to be interrupted at a particular
 * count, and paces its own work by that - which means a machine with no
 * audio at all still has to run this clock, or the game waits for a moment
 * that never comes. Shrek 2 gets through its start-up and stops here.
 *
 * Bit 0 runs it, bit 1 picks 48 kHz over 32 kHz, bit 2 lets the interrupt
 * through, bit 3 is the interrupt itself and is written back to clear it,
 * and bit 5 resets the count. */
#define GC_AI_RUN 0x0001u
#define GC_AI_48KHZ 0x0002u
#define GC_AI_INTERRUPT_ENABLE 0x0004u
#define GC_AI_INTERRUPT 0x0008u
#define GC_AI_RESET_COUNT 0x0020u

static uint32_t ai_sample_count(GcMachine *m) {
  const uint32_t *control;
  resolve_hot_slots(m);
  control = hot_slot(m, m->ai_control_slot);
  if (!control || !(*control & GC_AI_RUN)) return m->ai_counted_before;
  {
    const uint64_t elapsed = m->cpu.cycles - m->ai_started_cycles;
    const uint64_t rate = (*control & GC_AI_48KHZ) ? 48000ull : 32000ull;
    return m->ai_counted_before + (uint32_t)((elapsed * rate) / GC_CPU_HZ);
  }
}

static void ai_update_interrupt(GcMachine *m) {
  uint32_t *control;
  const uint32_t *timing;
  resolve_hot_slots(m);
  control = hot_slot(m, m->ai_control_slot);
  timing = hot_slot(m, m->ai_timing_slot);
  if (!control || !timing) return;
  /* The count has passed the moment the game asked about. */
  if ((*control & GC_AI_RUN) && *timing && ai_sample_count(m) >= *timing) {
    *control |= GC_AI_INTERRUPT;
  }
  gc_set_interrupt(m, GC_INT_AUDIO,
                   (*control & GC_AI_INTERRUPT) && (*control & GC_AI_INTERRUPT_ENABLE));
}

static uint32_t ai_sample_count_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  resolve_hot_slots(m);
  return ai_sample_count(m);
}

static void ai_sample_count_write(GcMachine *m, uint32_t value) {
  m->ai_counted_before = value;
  m->ai_started_cycles = m->cpu.cycles;
}

static void ai_control_write(GcMachine *m, uint32_t value) {
  uint32_t *control;
  resolve_hot_slots(m);
  control = hot_slot(m, m->ai_control_slot);
  if (!control) return;
  /* Whatever it was counting up to now, so that changing the rate or
   * stopping it does not lose the count. */
  m->ai_counted_before = ai_sample_count(m);
  m->ai_started_cycles = m->cpu.cycles;
  if (value & GC_AI_RESET_COUNT) m->ai_counted_before = 0;
  /* The interrupt bit is written back to clear, like every other one. The
   * store left it alone (REG_HOOK_W1C), so what is here is still whether it
   * was pending; the old expression read it after the store had already
   * replaced it with the written value, and a write of zero lost it. */
  *control &= ~(value & GC_AI_INTERRUPT);
  ai_update_interrupt(m);
}

/* Called as the machine runs, because the interrupt is a moment in time
 * rather than a consequence of anything the guest did. */
void gc_ai_advance(GcMachine *m) {
  resolve_hot_slots(m);
  ai_update_interrupt(m);
}

/* And the DSP's answer, when enough time has passed.
 *
 * The interrupt is raised here and not by the act of writing the mail,
 * which is the trap this protocol sets: on hardware the microcode raises
 * it as a separate act afterwards, so an implementation that assumes mail
 * interrupts by itself has a mailbox that works perfectly and a game that
 * never wakes up. */
void gc_dsp_advance(GcMachine *m) {
  uint32_t *control;
  if (!m->dsp_mail_queued || m->cpu.cycles < m->dsp_mail_due_cycles) return;
  if (m->dsp_mail_waiting) return; /* the CPU has not taken the last one */
  resolve_hot_slots(m);
  dsp_send_to_cpu(m, m->dsp_mail_queued);
  m->dsp_mail_queued = 0;
  /* AX's answers always interrupt. The Zelda family says which of its
   * mails do, and has the next one waiting behind this. */
  if (gc_zelda_active(m)) {
    const int interrupts = (int)gc_ax_zelda(m)->staged_interrupts;
    dsp_stage_zelda_mail(m);
    if (!interrupts) return;
  }
  control = hot_slot(m, m->dsp_control_slot);
  if (control) *control |= GC_DSP_DSP_INT;
  dsp_update_interrupt(m);
}

/* The four display interrupts the video interface can raise. Each has an
 * enable at bit 28 and a status at bit 31; the handler clears the status,
 * and the line drops when none is left set. */
static uint32_t *display_interrupt(GcMachine *m, unsigned index);

/* Called every time the beam moves a pixel - every thirty-six instructions
 * - so the registers are the remembered slots below, not a lookup by name.
 * It used to find them by name, four string comparisons and four scans of
 * the register table per call, and a sampling profile of Shrek 2 put that
 * at seven per cent of the whole run. */
void gc_vi_update_interrupt(GcMachine *m) {
  int asserted = 0;
  unsigned i;
  for (i = 0; i < 4; i++) {
    const uint32_t *slot = display_interrupt(m, i);
    if (slot && (*slot & 0x80000000u) && (*slot & 0x10000000u)) asserted = 1;
  }
  gc_set_interrupt(m, GC_INT_VIDEO, asserted);
}

/* The four display interrupt registers, found once and remembered. */
static uint32_t *display_interrupt(GcMachine *m, unsigned index) {
  static const uint32_t offsets[4] = {0x30, 0x34, 0x38, 0x3c};
  if (!m->vi_interrupt_slots_known) {
    unsigned i;
    for (i = 0; i < 4u; i++) {
      uint32_t *slot = io_slot(m, "vi", offsets[i]);
      m->vi_interrupt_slot[i] = slot ? (uint32_t)(slot - m->io) : 0xffffffffu;
    }
    m->vi_interrupt_slots_known = 1;
  }
  if (m->vi_interrupt_slot[index] == 0xffffffffu) return NULL;
  return &m->io[m->vi_interrupt_slot[index]];
}

/* What the video interface is scanning out, from its own registers.
 *
 * The television shows whatever buffer these name, however the pixels got
 * there. A game that draws with the graphics processor copies the frame
 * into that buffer first, but it does not have to: a video player decodes
 * straight into memory and points the display at it, and a screen reader
 * that only follows the copies shows nothing at all.
 *
 * The encoding is libogc's (video.c, __setFbbRegs and __setPicConfig):
 *  - +0x1c/+0x1e and +0x24/+0x26: the top and bottom fields' buffers. Bits
 *    7:0 of the high half and all of the low half are the address; bit 12
 *    says it was stored shifted right by five, which is how an address above
 *    sixteen megabytes fits in 24 bits.
 *  - +0x48: bits 7:0 the stride from one line of a field to the next, and
 *    bits 14:8 the words a line shows, both in units of sixteen pixels.
 *  - +0x00: bits 13:4 the active lines in a field.
 *  - +0x4a bit 12: horizontal scaling, with the buffer's width in +0x70.
 *
 * In the usual double-field layout the bottom field's buffer is one line
 * below the top's and the stride is two lines, so interleaving the fields
 * rebuilds the whole frame. When the two addresses are the same both fields
 * show the same lines and the frame is one field tall. */
int gc_vi_display(GcMachine *m, GcViDisplay *out) {
  const uint32_t *vertical = io_slot(m, "vi", 0x00);
  const uint32_t *top = io_slot(m, "vi", 0x1c);
  const uint32_t *bottom = io_slot(m, "vi", 0x24);
  const uint32_t *picture = io_slot(m, "vi", 0x48);
  const uint32_t *scaling = io_slot(m, "vi", 0x4a);
  const uint32_t *scaled_width = io_slot(m, "vi", 0x70);
  uint32_t lines, stride_words, shown_words;

  if (!vertical || !top || !bottom || !picture || !scaling || !scaled_width) return -1;
  lines = (*vertical >> 4) & 0x3ffu;
  stride_words = *picture & 0xffu;
  shown_words = (*picture >> 8) & 0x7fu;

  out->top = *top & 0x00ffffffu;
  if (*top & 0x10000000u) out->top <<= 5;
  out->bottom = *bottom & 0x00ffffffu;
  if (*top & 0x10000000u) out->bottom <<= 5;
  out->field_stride = stride_words * 32u;
  out->width = (*scaling & 0x1000u) ? (*scaled_width & 0x3ffu) : shown_words * 16u;
  out->height = (out->top != out->bottom) ? lines * 2u : lines;

  if (!out->top || !lines || !stride_words || !out->width) return -1;
  if (out->width > out->field_stride / 2u) out->width = out->field_stride / 2u;
  return 0;
}

/* The beam has moved from one position in the frame to another. Any
 * display interrupt whose target it passed becomes pending.
 *
 * A display interrupt is a *position* match, not a field event. Each
 * register names a line and a column, both counting from one, and fires
 * when the beam reaches them. The SDK uses two of them as the retrace: one
 * aimed at the top of the frame and one at the top of the second field,
 * which between them give one interrupt a field. The other two are for
 * raster effects, and a game that aims one at the middle of the screen
 * expects it in the middle of the screen - raising every armed interrupt
 * at the field boundary instead gets the retrace rate right by a wrong
 * route and puts every raster split about 240 lines early.
 *
 * A register whose line or column is still zero has never been programmed,
 * and a counter that starts at one never reaches zero, so it never
 * matches. That is the hardware's answer too and it matters: a game that
 * enables an interrupt before aiming it should get nothing, not a flood. */
static void vi_cross(GcMachine *m, uint32_t from, uint32_t to, int wrapped) {
  unsigned i;
  for (i = 0; i < 4u; i++) {
    uint32_t *slot = display_interrupt(m, i);
    uint32_t vertical, horizontal, target;
    if (!slot || !(*slot & 0x10000000u)) continue;
    vertical = (*slot >> 16) & 0x3ffu;
    horizontal = *slot & 0x3ffu;
    if (!vertical || !horizontal) continue;
    target = (vertical - 1u) * GC_VI_PIXELS_PER_LINE + (horizontal - 1u);
    if (target >= GC_VI_PIXELS_PER_FRAME) continue;
    /* Crossed, counting the position arrived at and not the one left. At
     * a frame boundary the interval is two pieces, the tail of the old
     * frame and the head of the new. */
    if (wrapped ? (target > from || target <= to) : (target > from && target <= to)) {
      *slot |= 0x80000000u;
    }
  }
  gc_vi_update_interrupt(m);
}

void gc_vi_beam(GcMachine *m, uint32_t previous, uint32_t now) {
  if (now == previous) return;
  vi_cross(m, previous, now, now < previous);
}

/* A frame boundary on its own raises nothing: the interrupts aimed there
 * are found by the position match above, like every other one. Kept so
 * that the first frame's target at position zero is not missed. */
void gc_vi_field_boundary(GcMachine *m) { vi_cross(m, 1u, 0u, 1); }

/* The earliest instruction count, no later than `next`, at which the
 * interfaces in this file could do something the guest would see: the beam
 * reaching a display interrupt's target, the audio clock reaching the count
 * a game asked about, the sound processor's mail coming due.
 *
 * Every answer here may be early and none may be late. Early costs one
 * look that finds nothing; late would deliver an interrupt after the
 * instruction it belongs before, which gc_timetest measures. So where the
 * arithmetic is awkward the answer is "the next instruction", and that is
 * always right. The machine has just been brought up to date when this is
 * asked (gc_machine_due in gc.c). */
uint64_t gc_io_next_event(GcMachine *m, uint64_t next) {
  const uint64_t now = m->cpu.cycles;
  const uint64_t per_pixel = GC_VI_CYCLES_PER_PIXEL;
  const uint64_t frame_start = (now / GC_VI_FRAME_CYCLES) * GC_VI_FRAME_CYCLES;
  const uint64_t pixel_now = (now - frame_start) / per_pixel;
  int asserted = 0;
  unsigned i;

  /* The display interrupts. The beam counts from the start of the frame, so
   * a target ahead of it is reached at a known count; one behind it is not
   * reached again until the next frame, and the field boundary that begins
   * it is an event of its own. Just after that boundary the beam has not
   * yet wrapped (see advance_video), and every target is crossed at the
   * next look. */
  if (m->vi_pixel > pixel_now) {
    if (now + 1u < next) next = now + 1u;
  }
  for (i = 0; i < 4u; i++) {
    const uint32_t *slot = display_interrupt(m, i);
    uint32_t vertical, horizontal, target;
    if (!slot) continue;
    if ((*slot & 0x80000000u) && (*slot & 0x10000000u)) asserted = 1;
    if (!(*slot & 0x10000000u)) continue;
    vertical = (*slot >> 16) & 0x3ffu;
    horizontal = *slot & 0x3ffu;
    if (!vertical || !horizontal) continue;
    target = (vertical - 1u) * GC_VI_PIXELS_PER_LINE + (horizontal - 1u);
    if (target >= GC_VI_PIXELS_PER_FRAME || target <= pixel_now) continue;
    if (frame_start + (uint64_t)target * per_pixel < next) {
      next = frame_start + (uint64_t)target * per_pixel;
    }
  }
  /* The line is recomputed from the registers every time the beam moves a
   * pixel. If the guest has acknowledged the line in the processor
   * interface while a display interrupt is still flagged, that recomputation
   * raises it again - at the next pixel, and so that is an event. */
  if (asserted != (int)((m->pi_cause >> GC_INT_VIDEO) & 1u)) {
    const uint64_t at = frame_start + (pixel_now + 1u) * per_pixel;
    if (at < next) next = at;
  }

  /* The audio clock. The count is `before + elapsed * rate / HZ` in 32 bits;
   * it reaches the target after exactly (target - count) more samples, and
   * the first instruction at which that many have been counted is a
   * ceiling division away. A count that would need more than 32 bits to
   * describe is left to the per-instruction check. */
  resolve_hot_slots(m);
  {
    const uint32_t *control = hot_slot(m, m->ai_control_slot);
    const uint32_t *timing = hot_slot(m, m->ai_timing_slot);
    if (control && timing && (*control & GC_AI_RUN) && *timing && !(*control & GC_AI_INTERRUPT)) {
      const uint32_t count = ai_sample_count(m);
      if (count >= *timing) {
        if (now < next) next = now;
      } else {
        const uint64_t rate = (*control & GC_AI_48KHZ) ? 48000ull : 32000ull;
        const uint64_t elapsed = now - m->ai_started_cycles;
        const uint64_t samples = (elapsed * rate) / GC_CPU_HZ + (uint64_t)(*timing - count);
        if (samples > 0xffffffffull) {
          if (now + 1u < next) next = now + 1u;
        } else {
          const uint64_t at = m->ai_started_cycles + (samples * GC_CPU_HZ + rate - 1u) / rate;
          if (at < next) next = at;
        }
      }
    }
  }

  /* Mail the sound processor owes, once the CPU has taken the last. */
  if (m->dsp_mail_queued && !m->dsp_mail_waiting && m->dsp_mail_due_cycles < next) {
    next = m->dsp_mail_due_cycles;
  }
  return next;
}

static void vi_display_interrupt_write(GcMachine *m, uint32_t value) {
  (void)value;
  gc_vi_update_interrupt(m);
}

/* ------------------------------------------------------------------ */
/* External interface                                                  */

/* EXI is a serial bus with three channels; the devices on it are the
 * memory cards, the SRAM that holds the console's settings and the real
 * time clock. Software starts a transfer by setting TSTART in a channel's
 * control register and then waits for the hardware to clear it - which is
 * where libogc's EXI_Sync sits, and where a machine that does not model
 * this stops for good.
 *
 * The transfer itself completes here with nothing on the other end: reads
 * come back as zeroes. That is enough for the settings code, which finds a
 * bad checksum, falls back to its defaults and carries on. Real devices are
 * P5's work, along with the memory card.
 */
static uint32_t *exi_slot(GcMachine *m, uint32_t offset);
static uint32_t *io_slot(GcMachine *m, const char *block_name, uint32_t offset);

/* ------------------------------------------------------------------ */
/* The console's own settings                                          */

/* Sixty-four bytes of battery-backed memory on the other end of EXI
 * channel 0, device 1 - the same device as the clock and the font ROM.
 * It holds what the console's menu lets somebody set: the language, the
 * video mode, the sound mode, the screen position, and a bias that turns
 * the clock into a date.
 *
 * A game reads this before it does anything else, and it checks the
 * checksum. Returning zeroes gives a checksum of zero over data of zero,
 * which does not match, and what a game does about that is its own
 * business - Shrek 2 stops asking for anything at all and spins. So this
 * is not an optional nicety; it is the first thing a console is asked
 * for, and it has to answer.
 *
 * The layout is the SDK's `syssram` followed by `syssramex`: two checksum
 * halfwords, two ethernet addresses, the clock bias, the screen offset,
 * the country, the language, a flags byte, and then the identifiers of
 * whatever peripherals have been plugged in - none, here.
 *
 * **Unverified:** the flags byte. Bit 2 is progressive scan and bits 0-1
 * are the sound mode on every description read; the rest are not
 * established, and this leaves them zero, which is what an unconfigured
 * console would have. */
#define GC_SRAM_SIZE 64u

/* The checksum the SDK computes: a sum and a sum of complements, over
 * everything after the two halfwords that hold them. */
static void sram_checksum(const uint8_t *sram, uint16_t *sum, uint16_t *complement) {
  unsigned i;
  *sum = 0;
  *complement = 0;
  for (i = 4u; i < GC_SRAM_SIZE; i += 2u) {
    const uint16_t word = (uint16_t)(((uint16_t)sram[i] << 8) | sram[i + 1u]);
    *sum = (uint16_t)(*sum + word);
    *complement = (uint16_t)(*complement + (uint16_t)~word);
  }
}

static void sram_image(const GcMachine *m, uint8_t out[GC_SRAM_SIZE]) {
  uint16_t sum, complement;
  unsigned i;
  uint8_t flash_sum = 0;
  memset(out, 0, GC_SRAM_SIZE);
  /* counter_bias, at 0x0c: zero, so the clock reads from 2000 rather than
   * from some other epoch. The two ethernet addresses above it stay zero;
   * nothing here has a broadband adapter. */
  out[0x10] = 0;    /* display offset, horizontal: centred */
  out[0x11] = 0;    /* country: the console this stands for is a retail one */
  out[0x12] = 0;    /* language: English */
  /* The flags byte. Bit 2 is the sound mode, and stereo is what a console
   * leaves the factory set to. The bit that matters more is the one that
   * is *not* set: bit 6 asks the console to boot into its own menu instead
   * of the disc, and a machine that has no menu and never will should not
   * be asking for one. The names are libogc's (SYS_GetSoundMode reads bit
   * 2, SYS_GetProgressiveScan reads bit 7); **Unverified:** whether the
   * retail library agrees about every bit, though it plainly agrees about
   * the two that are read here. */
  out[0x13] = 0x04;
  /* The flash identifiers, one per memory card slot, twelve bytes from 0x14
   * and from 0x20, and their checksums at 0x3a and 0x3b: the one's
   * complement of the byte-wise sum of the identifier. The card driver
   * compares the checksum before it reads a single block and refuses to
   * mount the card if it disagrees, and the retail library then checks the
   * card's header serial against the identifier itself and calls a card
   * that disagrees corrupted.
   *
   * On a console the unlock handshake with a card writes that card's
   * identifier here. This machine's card reports itself unlocked, so there
   * is no handshake and slot A's identifier is taken from the card in it
   * (gc_card_flash_id); an empty slot, and slot B, which is always empty,
   * read as zero with a checksum of 0xff. */
  gc_card_flash_id(m, out + 0x14);
  for (i = 0; i < 12u; i++) flash_sum = (uint8_t)(flash_sum + out[0x14 + i]);
  out[0x3a] = (uint8_t)~flash_sum;
  out[0x3b] = 0xff;
  sram_checksum(out, &sum, &complement);
  out[0] = (uint8_t)(sum >> 8);
  out[1] = (uint8_t)sum;
  out[2] = (uint8_t)(complement >> 8);
  out[3] = (uint8_t)complement;
}

/* What the last immediate write asked each channel's device for, and which
 * device the last chip-select picked, are the machine's (exi_command and
 * exi_selected in gc.h). They were file statics once, which two machines in
 * one process - the recompiled and interpreted pair of --differential -
 * shared, so each saw the other's selects. */

#define GC_EXI_DEVICE_CARD 1u     /* the memory card in this channel's slot */
#define GC_EXI_DEVICE_SETTINGS 2u /* the clock and the console's settings */

/* The status and control register, which is three different things at once.
 *
 * Most of it is the guest's own storage - the frequency, which device is
 * selected, and three mask bits. Four bits are not. Three of them are
 * interrupts the machine raises and the guest acknowledges by writing them
 * back *set*, which means an ordinary store must not be able to raise one -
 * so they are kept apart from the stored word and merged in on the way out.
 * The fourth says something is in the slot, and it is read-only.
 *
 * The masks sit exactly one bit below the interrupts they gate: bit 0 masks
 * bit 1, bit 2 masks bit 3, bit 10 masks bit 11. That is the whole of the
 * gating and it is why the test below is a single shift. */
#define GC_EXI_INTERRUPT 0x0002u /* the device wants attention */
#define GC_EXI_COMPLETE 0x0008u  /* the transfer finished */
#define GC_EXI_INSERTED 0x0800u  /* something was plugged in, or pulled out */
#define GC_EXI_STICKY (GC_EXI_INTERRUPT | GC_EXI_COMPLETE | GC_EXI_INSERTED)
/* Something is physically in this slot. It has to stay set for as long as
 * the card is in use: the library re-reads it after every deselect and after
 * every completed transfer, and a bit that drops in the middle of an
 * operation reads as the card having been pulled out. */
#define GC_EXI_PRESENT 0x1000u

static void exi_update_interrupt(GcMachine *m) {
  unsigned channel;
  int raised = 0;
  for (channel = 0; channel < 3u; channel++) {
    const uint32_t *slot = exi_slot(m, 0x14u * channel);
    if (!slot) continue;
    if (m->exi_status[channel] & (*slot << 1) & GC_EXI_STICKY) raised = 1;
  }
  gc_set_interrupt(m, GC_INT_EXI, raised);
}

static void exi_raise(GcMachine *m, unsigned channel, uint32_t bit) {
  if (channel >= 3u) return;
  m->exi_status[channel] |= bit;
  exi_update_interrupt(m);
}

/* A program or an erase has finished. This is the one the card cannot do
 * without: everything else it is asked to do completes when the transfer
 * does, and a write completes later, on its own account. A card that never
 * raises it reads perfectly and never saves. */
void gc_exi_card_interrupt(GcMachine *m) { exi_raise(m, 0, GC_EXI_INTERRUPT); }

static uint32_t exi_status_read(GcMachine *m, unsigned channel, uint32_t stored) {
  uint32_t value = (stored & ~(GC_EXI_STICKY | GC_EXI_PRESENT)) | m->exi_status[channel];
  if (channel == 0u && gc_card_present(m)) value |= GC_EXI_PRESENT;
  return value;
}

static void exi_status_write(GcMachine *m, unsigned channel, uint32_t value) {
  uint32_t *slot = exi_slot(m, 0x14u * channel);
  if (!slot) return;
  /* Writing an interrupt bit back set acknowledges it; writing it clear
   * leaves it alone. Neither can set one, so the stored word never carries
   * them at all. */
  m->exi_status[channel] &= ~(value & GC_EXI_STICKY);
  *slot &= ~(GC_EXI_STICKY | GC_EXI_PRESENT);
  if (channel == 0u) {
    /* Chip-select is the only framing a command has. There is no separate
     * strobe: a device learns it has been picked from this write and learns
     * its command has ended from the next one. */
    const unsigned device = (value >> 7) & 7u;
    if (device != m->exi_selected[0]) {
      m->exi_selected[0] = device;
      gc_card_select(m, device == GC_EXI_DEVICE_CARD);
    }
  }
  exi_update_interrupt(m);
}

static uint32_t exi_0_status_read(GcMachine *m, uint32_t stored) {
  return exi_status_read(m, 0, stored);
}
static uint32_t exi_1_status_read(GcMachine *m, uint32_t stored) {
  return exi_status_read(m, 1, stored);
}
static uint32_t exi_2_status_read(GcMachine *m, uint32_t stored) {
  return exi_status_read(m, 2, stored);
}
static void exi_0_status_write(GcMachine *m, uint32_t value) { exi_status_write(m, 0, value); }
static void exi_1_status_write(GcMachine *m, uint32_t value) { exi_status_write(m, 1, value); }
static void exi_2_status_write(GcMachine *m, uint32_t value) { exi_status_write(m, 2, value); }

static void exi_start(GcMachine *m, unsigned channel, uint32_t control) {
  const uint32_t base = 0x14u * channel;
  uint32_t *control_slot = exi_slot(m, base + 0x0c);
  uint32_t *data_slot = exi_slot(m, base + 0x10);
  const unsigned is_dma = (control >> 1) & 1u;
  const unsigned direction = (control >> 2) & 3u; /* 0 read, 1 write, 2 both */
  const unsigned length = ((control >> 4) & 3u) + 1u;
  const int card_here = channel == 0u && m->exi_selected[0] == GC_EXI_DEVICE_CARD;

  if (!control_slot || !data_slot) return;
  if ((control & 1u) == 0) return; /* not a start */

  if (is_dma) {
    /* A DMA transfer moves whole cache lines to or from main memory. */
    uint32_t *address_slot = exi_slot(m, base + 0x04);
    uint32_t *length_slot = exi_slot(m, base + 0x08);
    uint8_t *p = NULL;
    if (address_slot && length_slot) {
      p = gc_ram_at_register(m, *address_slot, *length_slot);
    }
    if (!p) return;
    if (card_here) {
      if (direction == 0) memset(p, 0, *length_slot);
      gc_card_dma(m, p, *length_slot, direction == 1u);
    } else if (direction == 0) {
      memset(p, 0, *length_slot);
      /* The settings, when this is what was asked for. Command 0x20000100
       * on channel 0 selects them; anything else still reads as nothing,
       * because nothing else is attached. */
      if (channel == 0u && m->exi_command[0] == 0x20000100u) {
        uint8_t sram[GC_SRAM_SIZE];
        const uint32_t take = (*length_slot < GC_SRAM_SIZE) ? *length_slot : GC_SRAM_SIZE;
        sram_image(m, sram);
        memcpy(p, sram, take);
      }
    }
  } else if (direction == 0) {
    if (!card_here || !gc_card_immediate_read(m, length, data_slot)) *data_slot = 0;
  } else if (channel < 3u) {
    /* A write of the command word, remembered for the read that follows. */
    m->exi_command[channel] = *data_slot;
    if (card_here) gc_card_command_bytes(m, *data_slot, length);
  }

  /* The hardware clears TSTART when the transfer finishes; here it has
   * already finished. And a finished transfer raises its own interrupt,
   * which is how the guest learns that a bulk read landed - the short
   * commands are waited for by spinning on TSTART instead, so they never
   * reach the handler. */
  *control_slot = control & ~1u;
  exi_raise(m, channel, GC_EXI_COMPLETE);
}

static void exi_0_control(GcMachine *m, uint32_t value) { exi_start(m, 0, value); }
static void exi_1_control(GcMachine *m, uint32_t value) { exi_start(m, 1, value); }
static void exi_2_control(GcMachine *m, uint32_t value) { exi_start(m, 2, value); }

/* ------------------------------------------------------------------ */
/* The blocks                                                          */

/* ------------------------------------------------------------------ */
/* The disc drive                                                      */

/* Software writes a command into three registers, says where the answer
 * goes, and sets TSTART; the drive clears TSTART when it is done and raises
 * the transfer-complete interrupt. A real drive takes milliseconds over
 * this. Here it finishes inside the instruction that started it, because
 * nothing can observe it in progress: the interrupt is what software waits
 * on, and it arrives.
 *
 * Seek times are a P8 question. Modelling them is not free accuracy - a
 * game that streams audio off the disc is timed against a real drive, and
 * making one up is as likely to be wrong as making none up. When there is
 * something to measure against, it gets measured.
 */
enum {
  GC_DI_BRK = 0x001u,
  GC_DI_DEINT_MASK = 0x002u,
  GC_DI_DEINT = 0x004u,
  GC_DI_TCINT_MASK = 0x008u,
  GC_DI_TCINT = 0x010u,
  GC_DI_BRKINT_MASK = 0x020u,
  GC_DI_BRKINT = 0x040u
};
enum { GC_DI_COVER_OPEN = 0x001u, GC_DI_CVRINT_MASK = 0x002u, GC_DI_CVRINT = 0x004u };

/* The error codes software asks for with 0xe0, as YAGCD records them. */
#define GC_DI_NO_ERROR 0x00000000u
#define GC_DI_NO_DISC 0x00023a00u
#define GC_DI_PAST_THE_END 0x00056300u

static void di_update_interrupt(GcMachine *m) {
  const uint32_t *status = io_slot(m, "di", 0x00);
  const uint32_t *cover = io_slot(m, "di", 0x04);
  int asserted = 0;
  if (status) {
    const uint32_t s = *status;
    if ((s & GC_DI_DEINT) && (s & GC_DI_DEINT_MASK)) asserted = 1;
    if ((s & GC_DI_TCINT) && (s & GC_DI_TCINT_MASK)) asserted = 1;
    if ((s & GC_DI_BRKINT) && (s & GC_DI_BRKINT_MASK)) asserted = 1;
  }
  if (cover && (*cover & GC_DI_CVRINT) && (*cover & GC_DI_CVRINT_MASK)) asserted = 1;
  gc_set_interrupt(m, GC_INT_DVD, asserted);
}

/* The three interrupt bits are written back to clear; the masks are stored.
 * The generic path has already put the written value in the slot, so this
 * puts the register back the way it was except where a one asked for a bit
 * to go. */
static void di_status_write(GcMachine *m, uint32_t value) {
  uint32_t *slot = io_slot(m, "di", 0x00);
  if (!slot) return;
  *slot = (m->di_status & ~(value & (GC_DI_DEINT | GC_DI_TCINT | GC_DI_BRKINT))) |
          (value & (GC_DI_DEINT_MASK | GC_DI_TCINT_MASK | GC_DI_BRKINT_MASK | GC_DI_BRK));
  m->di_status = *slot;
  di_update_interrupt(m);
}

/* The cover is closed when there is a disc in the drive and open when there
 * is not, which is how software decides whether to wait for one. */
static uint32_t di_cover_read(GcMachine *m, uint32_t stored) {
  return (stored & ~GC_DI_COVER_OPEN) | (m->disc ? 0u : GC_DI_COVER_OPEN);
}

static void di_cover_write(GcMachine *m, uint32_t value) {
  uint32_t *slot = io_slot(m, "di", 0x04);
  if (!slot) return;
  *slot = (m->di_cover & ~(value & GC_DI_CVRINT)) | (value & GC_DI_CVRINT_MASK);
  m->di_cover = *slot;
  di_update_interrupt(m);
}

/* A DMA moves through memory: the address register walks forward and the
 * length register counts down to zero. Software reads the length back to
 * find out how much arrived - libogc's interrupt handler computes the
 * transferred size as "asked for, minus what is left", and a drive that
 * leaves the register alone tells it that nothing moved. It then reissues
 * the same read, forever.
 *
 * Both registers hold physical addresses, and software writes a cached
 * effective address into the first of them, so the top bits go. */
static void di_advance(GcMachine *m, uint32_t length) {
  uint32_t *address = io_slot(m, "di", 0x14);
  uint32_t *left = io_slot(m, "di", 0x18);
  if (address) *address += length;
  if (left) *left = (*left > length) ? (*left - length) : 0u;
}

/* The answer to a command goes either into memory by DMA or into the
 * immediate register, depending on the bit software set in the control
 * register. */
static int di_deliver(GcMachine *m, const uint8_t *bytes, uint32_t length, int is_dma) {
  if (!is_dma) {
    uint32_t *immediate = io_slot(m, "di", 0x20);
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < 4 && i < length; i++) value = (value << 8) | bytes[i];
    if (immediate) *immediate = value;
    return 0;
  }
  {
    const uint32_t *address = io_slot(m, "di", 0x14);
    uint8_t *destination = address ? gc_ram_at_register(m, *address, length) : NULL;
    if (!destination) return -1;
    memcpy(destination, bytes, length);
    di_advance(m, length);
    return 0;
  }
}

/* What the drive says about itself when asked: the numbers YAGCD records
 * for a retail drive. Nothing here comes from a console's firmware
 * (docs/GC-PLAN.md, "No copyrighted firmware"). */
static void di_drive_info(uint8_t out[32]) {
  memset(out, 0, 32);
  out[0] = 0x00; out[1] = 0x02;                 /* revision level, halfword */
  out[2] = 0x00; out[3] = 0x01;                 /* device code, halfword */
  out[4] = 0x20; out[5] = 0x01;                 /* release date, 2001-06-08 */
  out[6] = 0x06; out[7] = 0x08;
}

static int di_read(GcMachine *m, uint64_t offset, uint32_t length, int is_dma) {
  const uint32_t *address = io_slot(m, "di", 0x14);
  uint8_t *destination;
  if (!m->disc) return -1;
  /* Every read a game issues is a DMA. An immediate one would be four
   * bytes, and saying it is not done is better than half-doing it. */
  if (!is_dma) return -1;
  destination = address ? gc_ram_at_register(m, *address, length) : NULL;
  if (!destination) return -1;
  /* The file read goes into main memory from inside the operating system,
   * which fails on a page still held for a copy (gc_memory.h) instead of
   * waiting for it. */
  gc_memory_settle(m->mem1, gc_ram_from_register(*address), length);
  if (gc_disc_read(m->disc, offset, destination, length) != 0) return -1;
  di_advance(m, length);
  return 0;
}

static void di_control_write(GcMachine *m, uint32_t value) {
  uint32_t *control = io_slot(m, "di", 0x1c);
  uint32_t *status = io_slot(m, "di", 0x00);
  const uint32_t *cmd0 = io_slot(m, "di", 0x08);
  const uint32_t *cmd1 = io_slot(m, "di", 0x0c);
  const uint32_t *cmd2 = io_slot(m, "di", 0x10);
  const int is_dma = (value & 2u) != 0;
  int failed = 0;

  if (!control || !status || !cmd0 || !cmd1 || !cmd2) return;
  if ((value & 1u) == 0) return; /* not a start */

  switch (*cmd0 >> 24) {
    case 0xa8: /* read: an offset in units of four bytes, and a length */
      failed = di_read(m, (uint64_t)*cmd1 << 2, *cmd2, is_dma) != 0;
      m->disc_error = failed ? (m->disc ? GC_DI_PAST_THE_END : GC_DI_NO_DISC) : GC_DI_NO_ERROR;
      break;
    case 0x12: { /* inquiry: what drive is this */
      uint8_t info[32];
      const uint32_t length = *cmd2 < sizeof(info) ? *cmd2 : (uint32_t)sizeof(info);
      di_drive_info(info);
      failed = di_deliver(m, info, length, is_dma) != 0;
      m->disc_error = GC_DI_NO_ERROR;
      break;
    }
    case 0xe0: { /* what went wrong last time */
      const uint8_t error[4] = {(uint8_t)(m->disc_error >> 24), (uint8_t)(m->disc_error >> 16),
                                (uint8_t)(m->disc_error >> 8), (uint8_t)m->disc_error};
      di_deliver(m, error, sizeof(error), is_dma);
      m->disc_error = GC_DI_NO_ERROR; /* asking clears it, as the drive does */
      break;
    }
    case 0xab: /* seek: there is nowhere to move a head to */
    case 0xe1: /* audio streaming, which is P5 */
    case 0xe2:
    case 0xe3: /* stop the motor */
    case 0xe4: /* audio buffer configuration */
    case 0x55: /* enable the debug extensions */
      break;
    default:
      /* Unlocking the drive, and the maker's own commands, land here. They
       * are answered as if they worked, because refusing one stops the
       * start-up that issued it - but they are counted and named, so a game
       * leaning on something real is visible rather than silent. */
      m->unknown_disc_commands++;
      if (m->log && m->unknown_disc_commands <= 8) {
        fprintf(m->log, "gc: disc command %08x %08x %08x, pc %08x\n", *cmd0, *cmd1, *cmd2,
                m->cpu.pc);
      }
      break;
  }

  /* The drive clears TSTART when it is finished, and raises one of two
   * interrupts: the transfer completed, or there is an error to report. */
  *control = value & ~1u;
  *status |= failed ? GC_DI_DEINT : GC_DI_TCINT;
  m->di_status = *status;
  di_update_interrupt(m);
}

/* ------------------------------------------------------------------ */
/* The command processor and the pixel engine                          */

/* Every one of these registers is a 32-bit value software writes as two
 * halfwords, low first, so a hook reads both slots back rather than trusting
 * the half it was handed. */
static uint32_t cp_pair(GcMachine *m, uint32_t offset) {
  const uint32_t *low = io_slot(m, "cp", offset);
  const uint32_t *high = io_slot(m, "cp", offset + 2u);
  return ((high ? *high : 0u) << 16) | (low ? *low & 0xffffu : 0u);
}

/* The ring's base and end. Both are physical addresses with the top bits
 * already gone, because that is how software writes them. */
static void cp_base_write(GcMachine *m, uint32_t value) {
  (void)value;
  m->gx.fifo_base = gc_ram_from_register(cp_pair(m, 0x20)) & ~31u;
}

static void cp_end_write(GcMachine *m, uint32_t value) {
  (void)value;
  m->gx.fifo_end = gc_ram_from_register(cp_pair(m, 0x24));
}

/* The write and read pointers. The processor interface has its own view of
 * the write pointer and the command processor has this one; while the two
 * FIFOs are linked they are the same pointer, and this is where it lives. */
static void cp_write_ptr_write(GcMachine *m, uint32_t value) {
  (void)value;
  m->gx.write_ptr = gc_ram_from_register(cp_pair(m, 0x34)) & ~31u;
}

static void cp_read_ptr_write(GcMachine *m, uint32_t value) {
  (void)value;
  m->gx.read_ptr = gc_ram_from_register(cp_pair(m, 0x38)) & ~31u;
}

/* A read hook is not told which register it is answering for, so each half
 * gets its own. */
static uint32_t cp_write_low_read(GcMachine *m, uint32_t s) { (void)s; return gc_ram_to_register(m->gx.write_ptr) & 0xffffu; }
static uint32_t cp_write_high_read(GcMachine *m, uint32_t s) { (void)s; return gc_ram_to_register(m->gx.write_ptr) >> 16; }
static uint32_t cp_read_low_read(GcMachine *m, uint32_t s) { (void)s; return gc_ram_to_register(m->gx.read_ptr) & 0xffffu; }
static uint32_t cp_read_high_read(GcMachine *m, uint32_t s) { (void)s; return gc_ram_to_register(m->gx.read_ptr) >> 16; }

/* How far the graphics processor is behind. Software watches this to decide
 * whether there is room for more. */
static uint32_t gx_distance(const GcMachine *m) {
  const uint32_t limit = (m->gx.fifo_end & ~31u) + 32u;
  const uint32_t span = (limit > m->gx.fifo_base) ? (limit - m->gx.fifo_base) : 0u;
  if (!span) return 0;
  return (m->gx.write_ptr + span - m->gx.read_ptr) % span;
}

static uint32_t cp_distance_low_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return gx_distance(m) & 0xffffu;
}

static uint32_t cp_distance_high_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return gx_distance(m) >> 16;
}

/* The status register. Reading it is how software waits for the graphics
 * processor to catch up, and this one always has: a burst is executed
 * inside the store that delivers it, because nothing between here and the
 * pixel engine can observe it half done. Real timing is P8's, and will be
 * measured rather than invented. */
static uint32_t cp_status_read(GcMachine *m, uint32_t stored) {
  const int idle = m->gx.read_ptr == m->gx.write_ptr;
  uint32_t value = stored & 0x03u; /* the overflow and underflow flags */
  if (idle) value |= 0x04u | 0x08u; /* read idle, command idle */
  return value;
}

/* Bit 0 lets the graphics processor read; bit 4 links the two FIFOs, so
 * that what the processor writes is what the graphics processor is given.
 * A game sets both to draw, and takes the link away to write a display list
 * into memory for later (gc_gx.h, GC_GX_UNLINKED). */
static uint32_t *pi_fifo_register(GcMachine *m, unsigned which);

static void cp_control_write(GcMachine *m, uint32_t value) {
  const int was_linked = !(m->gx.linked & GC_GX_UNLINKED);
  m->gx.linked = ((value & 0x01u) ? GC_GX_READS : 0) | ((value & 0x10u) ? 0 : GC_GX_UNLINKED);
  if (was_linked && (m->gx.linked & GC_GX_UNLINKED)) {
    /* The link goes: the processor's write pointer is its own from here,
     * and starts where the two had got to together. */
    uint32_t *write = pi_fifo_register(m, 2);
    if (write) *write = gc_ram_to_register(m->gx.write_ptr);
  }
  gc_gx_run(m);
}

/* The processor's own FIFO: where the write-gather pipe's bursts land.
 *
 * It has a base, an end and a write pointer of its own, in the processor
 * interface, and they are kept in those three registers and nowhere else.
 * While the two FIFOs are linked the game has set both to the same ring and
 * a burst moves both write pointers, so the graphics processor's is the one
 * kept and this one reads as it (as it always did here: the register's own
 * slot is not written on every burst, which would change what a save state
 * of a GameCube game holds for no gain). Unlinked, a burst is only bytes in
 * memory at this pointer (gc_pi_fifo_store): that is how the SDK's
 * GXBeginDisplayList records commands instead of running them.
 *
 * The three registers are found once for the process, not once a machine:
 * every machine in a process has the same register table, and the machine's
 * structure has no room to remember them. */
static uint32_t *pi_fifo_register(GcMachine *m, unsigned which) {
  static const uint32_t OFFSET[3] = {0x0c, 0x10, 0x14};
  static uint32_t slot[3];
  static int known;
  if (!known) {
    unsigned i;
    for (i = 0; i < 3u; i++) {
      const uint32_t *found = io_slot(m, "pi", OFFSET[i]);
      slot[i] = found ? (uint32_t)(found - m->io) : GC_SLOT_MISSING;
    }
    known = 1;
  }
  return slot[which] == GC_SLOT_MISSING ? NULL : &m->io[slot[which]];
}

/* How much has been recorded that way: for the runner's summary. A count
 * for the process, like the performance counters (gc_perf.h). */
static uint64_t pi_fifo_recorded;

uint64_t gc_pi_fifo_recorded(void) { return pi_fifo_recorded; }

void gc_pi_fifo_store(GcMachine *m, const uint8_t *bytes, unsigned size) {
  const uint32_t *base = pi_fifo_register(m, 0);
  const uint32_t *end = pi_fifo_register(m, 1);
  uint32_t *write = pi_fifo_register(m, 2);
  uint32_t at;
  uint8_t *p;
  if (!base || !end || !write) return;
  at = gc_ram_from_register(*write) & ~31u;
  p = gc_ram_ptr(m, at, size);
  if (p) memcpy(p, bytes, size);
  pi_fifo_recorded += size;
  /* The pointer wraps after the block the end register names. */
  at = (at == (gc_ram_from_register(*end) & ~31u)) ? (gc_ram_from_register(*base) & ~31u) : at + size;
  *write = gc_ram_to_register(at);
}

static uint32_t pi_fifo_write_ptr_read(GcMachine *m, uint32_t stored) {
  if (!(m->gx.linked & GC_GX_UNLINKED)) return gc_ram_to_register(m->gx.write_ptr);
  return gc_ram_to_register(gc_ram_from_register(stored) & ~31u);
}

/* The pixel engine's two interrupts. Bits 0 and 1 enable them; bits 2 and 3
 * are their status, written back to clear, the same shape the disc drive's
 * status register has. */
static void pe_update_interrupt(GcMachine *m) {
  const uint32_t *slot = io_slot(m, "pe", 0x0a);
  const uint32_t c = slot ? *slot : 0u;
  gc_set_interrupt(m, GC_INT_PE_TOKEN, (c & 1u) && (c & 4u));
  gc_set_interrupt(m, GC_INT_PE_FINISH, (c & 2u) && (c & 8u));
}

static void pe_control_write(GcMachine *m, uint32_t value) {
  uint32_t *slot = io_slot(m, "pe", 0x0a);
  if (!slot) return;
  *slot = (m->pe_control & ~(value & 0x0cu)) | (value & 0x03u);
  m->pe_control = *slot;
  pe_update_interrupt(m);
}

void gc_pe_token(GcMachine *m, uint16_t token, int interrupt) {
  uint32_t *value = io_slot(m, "pe", 0x0e);
  uint32_t *control = io_slot(m, "pe", 0x0a);
  if (value) *value = token;
  if (interrupt && control) {
    *control |= 0x04u;
    m->pe_control = *control;
    pe_update_interrupt(m);
  }
}

void gc_pe_finish(GcMachine *m) {
  uint32_t *control = io_slot(m, "pe", 0x0a);
  if (!control) return;
  *control |= 0x08u;
  m->pe_control = *control;
  pe_update_interrupt(m);
}

#define REG(offset, width, name) {(offset), (width), (name), 0xffffffffu, 0xffffffffu, NULL, NULL}
#define REG_RO(offset, width, name, read) \
  {(offset), (width), (name), 0xffffffffu, 0u, (read), NULL}
#define REG_HOOK(offset, width, name, read, write) \
  {(offset), (width), (name), 0xffffffffu, 0xffffffffu, (read), (write)}
/* A register with interrupt status bits that software acknowledges by writing
 * a one. The store must not touch them at all - a write that acknowledges one
 * interrupt carries zeroes in the others, and storing those zeroes loses an
 * interrupt that is still pending. The hook clears what was written as one. */
#define REG_HOOK_W1C(offset, width, name, read, write, status) \
  {(offset), (width), (name), 0xffffffffu, ~(uint32_t)(status), (read), (write)}

/* Command processor: the ring the graphics processor reads from. */
static const GcIoReg cp_regs[] = {
    REG_HOOK(0x00, 2, "cp_status", cp_status_read, NULL),
    REG_HOOK(0x02, 2, "cp_control", NULL, cp_control_write),
    REG(0x04, 2, "cp_clear"),
    REG(0x06, 2, "cp_perf_select"),
    REG_HOOK(0x20, 2, "cp_fifo_base_lo", NULL, cp_base_write),
    REG_HOOK(0x22, 2, "cp_fifo_base_hi", NULL, cp_base_write),
    REG_HOOK(0x24, 2, "cp_fifo_end_lo", NULL, cp_end_write),
    REG_HOOK(0x26, 2, "cp_fifo_end_hi", NULL, cp_end_write),
    REG(0x28, 2, "cp_fifo_hiwater_lo"), REG(0x2a, 2, "cp_fifo_hiwater_hi"),
    REG(0x2c, 2, "cp_fifo_lowater_lo"), REG(0x2e, 2, "cp_fifo_lowater_hi"),
    REG_HOOK(0x30, 2, "cp_fifo_distance_lo", cp_distance_low_read, NULL),
    REG_HOOK(0x32, 2, "cp_fifo_distance_hi", cp_distance_high_read, NULL),
    REG_HOOK(0x34, 2, "cp_fifo_write_lo", cp_write_low_read, cp_write_ptr_write),
    REG_HOOK(0x36, 2, "cp_fifo_write_hi", cp_write_high_read, cp_write_ptr_write),
    REG_HOOK(0x38, 2, "cp_fifo_read_lo", cp_read_low_read, cp_read_ptr_write),
    REG_HOOK(0x3a, 2, "cp_fifo_read_hi", cp_read_high_read, cp_read_ptr_write),
    REG(0x3c, 2, "cp_fifo_breakpoint_lo"), REG(0x3e, 2, "cp_fifo_breakpoint_hi"),
};

/* Pixel engine: the end-of-frame interrupts a renderer raises. */
/* Pixel engine: the end of the pipeline, and the two interrupts a game
 * waits on to know the graphics processor has caught up. */
static const GcIoReg pe_regs[] = {
    REG(0x00, 2, "pe_zconfig"), REG(0x02, 2, "pe_alphaconf"),
    REG(0x04, 2, "pe_destination_alpha"), REG(0x06, 2, "pe_alpha_mode"),
    REG(0x08, 2, "pe_alpha_read"),
    REG_HOOK(0x0a, 2, "pe_control", NULL, pe_control_write),
    REG(0x0c, 2, "pe_token_unused"), REG(0x0e, 2, "pe_token"),
    REG(0x10, 2, "pe_bbox_left"), REG(0x12, 2, "pe_bbox_right"),
    REG(0x14, 2, "pe_bbox_top"), REG(0x16, 2, "pe_bbox_bottom"),
};

/* Video interface. Most of it is timing the guest programs and reads back;
 * the beam position is the one register with behaviour. */
static const GcIoReg vi_regs[] = {
    REG(0x00, 2, "vi_vertical_timing"), REG(0x02, 2, "vi_display_config"),
    REG(0x04, 4, "vi_horizontal_timing_0"), REG(0x08, 4, "vi_horizontal_timing_1"),
    REG(0x0c, 4, "vi_vertical_timing_odd"), REG(0x10, 4, "vi_vertical_timing_even"),
    REG(0x14, 4, "vi_burst_blanking_odd"), REG(0x18, 4, "vi_burst_blanking_even"),
    REG(0x1c, 4, "vi_framebuffer_top"), REG(0x20, 4, "vi_framebuffer_top_right"),
    REG(0x24, 4, "vi_framebuffer_bottom"), REG(0x28, 4, "vi_framebuffer_bottom_right"),
    REG_RO(0x2c, 4, "vi_beam_position", vi_position_read),
    REG_HOOK(0x30, 4, "vi_display_interrupt_0", NULL, vi_display_interrupt_write),
    REG_HOOK(0x34, 4, "vi_display_interrupt_1", NULL, vi_display_interrupt_write),
    REG_HOOK(0x38, 4, "vi_display_interrupt_2", NULL, vi_display_interrupt_write),
    REG_HOOK(0x3c, 4, "vi_display_interrupt_3", NULL, vi_display_interrupt_write),
    REG(0x40, 4, "vi_display_latch_0"), REG(0x44, 4, "vi_display_latch_1"),
    REG(0x48, 2, "vi_picture_config"), REG(0x4a, 2, "vi_horizontal_scaling"),
    REG(0x4c, 4, "vi_filter_coefficient_0"), REG(0x50, 4, "vi_filter_coefficient_1"),
    REG(0x54, 4, "vi_filter_coefficient_2"), REG(0x58, 4, "vi_filter_coefficient_3"),
    REG(0x5c, 4, "vi_filter_coefficient_4"), REG(0x60, 4, "vi_filter_coefficient_5"),
    REG(0x64, 4, "vi_anti_alias"), REG(0x68, 2, "vi_clock"),
    REG(0x6a, 2, "vi_dtv_status"), REG(0x6c, 2, "vi_border_hblank_start"),
    REG(0x6e, 2, "vi_border_hblank_end"), REG(0x70, 2, "vi_scaling_width"),
    REG(0x72, 2, "vi_unknown_72"),
};

/* Processor interface: interrupts, and the FIFO the CPU writes through. */
static const GcIoReg pi_regs[] = {
    REG_HOOK(0x00, 4, "pi_interrupt_cause", pi_cause_read, pi_cause_write),
    REG_HOOK(0x04, 4, "pi_interrupt_mask", pi_mask_read, pi_mask_write),
    REG(0x0c, 4, "pi_fifo_base_start"),
    REG(0x10, 4, "pi_fifo_base_end"),
    REG_HOOK(0x14, 4, "pi_fifo_write_pointer", pi_fifo_write_ptr_read, NULL),
    REG(0x24, 4, "pi_reset"),
    REG_RO(0x2c, 4, "pi_console_type", pi_console_read),
};

/* Memory interface: the protection ranges and its own interrupts. */
static const GcIoReg mi_regs[] = {
    REG(0x00, 2, "mi_protect_region_0_lo"), REG(0x02, 2, "mi_protect_region_0_hi"),
    REG(0x04, 2, "mi_protect_region_1_lo"), REG(0x06, 2, "mi_protect_region_1_hi"),
    REG(0x08, 2, "mi_protect_region_2_lo"), REG(0x0a, 2, "mi_protect_region_2_hi"),
    REG(0x0c, 2, "mi_protect_region_3_lo"), REG(0x0e, 2, "mi_protect_region_3_hi"),
    REG(0x10, 2, "mi_protect_type"), REG(0x1c, 2, "mi_interrupt_mask"),
    REG(0x1e, 2, "mi_interrupt_cause"), REG(0x20, 2, "mi_address_lo"),
    REG(0x22, 2, "mi_address_hi"), REG(0x28, 2, "mi_timer"),
};

/* The audio DMA's two hooks, and the interrupt it raises when a buffer runs
 * out. The interrupt is the sound processor's rather than the audio
 * interface's own, which is confusing and is the hardware's arrangement:
 * the audio interface has a separate interrupt for its sample counter. */
static void audio_dma_length_write(GcMachine *m, uint32_t value) {
  gc_audio_length_write(m, value);
}

static uint32_t audio_dma_left_read(GcMachine *m, uint32_t stored) {
  (void)stored;
  return gc_audio_blocks_left(m);
}

void gc_dsp_audio_interrupt(GcMachine *m) {
  uint32_t *control = io_slot(m, "dsp", 0x0a);
  if (!control) return;
  *control |= GC_DSP_AUDIO_INT;
  dsp_update_interrupt(m);
}

/* The register storage, by name, for the parts of the machine that live in
 * their own translation units. */
uint32_t *gc_io_slot(GcMachine *m, const char *block_name, uint32_t offset) {
  return io_slot(m, block_name, offset);
}

/* DSP interface: the mailboxes, the control register and the ARAM and audio
 * DMA the sound code drives. */
static const GcIoReg dsp_regs[] = {
    REG_HOOK(0x00, 2, "dsp_mailbox_out_hi", dsp_mail_out_hi_read, NULL),
    REG_HOOK(0x02, 2, "dsp_mailbox_out_lo", NULL, dsp_mail_out_lo_write),
    REG_RO(0x04, 2, "dsp_mailbox_in_hi", dsp_mail_in_hi_read),
    REG_RO(0x06, 2, "dsp_mailbox_in_lo", dsp_mail_in_lo_read),
    REG_HOOK_W1C(0x0a, 2, "dsp_control", dsp_control_read, dsp_control_write,
                 GC_DSP_AUDIO_INT | GC_DSP_ARAM_INT | GC_DSP_DSP_INT),
    REG(0x12, 2, "aram_size"), REG_HOOK(0x16, 2, "aram_mode", aram_mode_read, NULL),
    /* The refresh rate the memory controller runs ARAM at. A game sets
     * it during start-up and reads it back; nothing here depends on the
     * value, but a register that answers nothing is a register a game
     * can wait on. */
    REG(0x1a, 2, "aram_refresh"),
    REG(0x20, 2, "aram_dma_main_hi"), REG(0x22, 2, "aram_dma_main_lo"),
    REG(0x24, 2, "aram_dma_aram_hi"), REG(0x26, 2, "aram_dma_aram_lo"),
    REG(0x28, 2, "aram_dma_length_hi"),
    REG_HOOK(0x2a, 2, "aram_dma_length_lo", NULL, aram_dma_start),
    /* The audio DMA. The address is split across two registers because the
     * guest writes it as two halves, and the length register's top bit is
     * what starts and stops the engine - so that is where the hook goes. */
    REG(0x30, 2, "audio_dma_start_hi"), REG(0x32, 2, "audio_dma_start_lo"),
    REG_HOOK(0x36, 2, "audio_dma_length", NULL, audio_dma_length_write),
    REG_RO(0x3a, 2, "audio_dma_left", audio_dma_left_read),
};

/* DVD interface: three command registers, where the answer goes, and a
 * control register whose start bit runs it. */
static const GcIoReg di_regs[] = {
    REG_HOOK(0x00, 4, "di_status", NULL, di_status_write),
    REG_HOOK(0x04, 4, "di_cover", di_cover_read, di_cover_write),
    REG(0x08, 4, "di_command_0"), REG(0x0c, 4, "di_command_1"), REG(0x10, 4, "di_command_2"),
    REG(0x14, 4, "di_dma_address"), REG(0x18, 4, "di_dma_length"),
    REG_HOOK(0x1c, 4, "di_control", NULL, di_control_write), REG(0x20, 4, "di_immediate"),
    REG(0x24, 4, "di_configuration"),
};

/* ------------------------------------------------------------------ */
/* The controller ports                                                */

/* The transfer-complete interrupt, which is bit 31 with its mask at bit
 * 30 - the same shape as every other interrupt in this machine. */
static void gc_si_update_interrupt(GcMachine *m) {
  const uint32_t *slot = io_slot(m, "si", 0x34);
  gc_set_interrupt(m, GC_INT_SERIAL,
                   slot && (*slot & 0x80000000u) && (*slot & 0x40000000u));
}

/* What a standard controller says it is: a Dolphin-type device, standard
 * variety. A game asks for this before it asks for anything else, and a
 * port that answers nothing is a port with nothing in it. */
#define GC_SI_TYPE_STANDARD 0x09000000u

/* Per-channel status, packed a byte to a channel with channel 0 highest.
 * Only two of the six bits arise here: something answered, or nothing
 * did. */
#define GC_SI_READ_STATUS 0x20u
#define GC_SI_NO_RESPONSE 0x08u

static GcPad *pad_of(GcMachine *m, unsigned channel) {
  if (!m->pads || channel >= GC_PAD_PORTS) return NULL;
  if (!m->pads[channel].present) return NULL;
  return &m->pads[channel];
}

/* One controller's state, in the eight bytes a poll returns.
 *
 * The sticks arrive here centred on zero because that is what a caller
 * finds natural, and leave biased by 128 because that is what the hardware
 * reports - a controller at rest reads 128, not 0. Getting that backwards
 * gives a game a stick held hard to one corner from the moment it starts,
 * which it will read as the player doing that. */
static void pad_poll_reply(const GcPad *pad, uint8_t out[8]) {
  out[0] = (uint8_t)(pad->buttons >> 8);
  out[1] = (uint8_t)pad->buttons;
  out[2] = (uint8_t)(pad->stick_x + 128);
  out[3] = (uint8_t)(pad->stick_y + 128);
  out[4] = (uint8_t)(pad->c_stick_x + 128);
  out[5] = (uint8_t)(pad->c_stick_y + 128);
  out[6] = pad->trigger_l;
  out[7] = pad->trigger_r;
}

/* The origin: where the sticks sit when nobody is touching them. A game
 * reads this once and subtracts it from everything afterwards, so it has
 * to agree with what a resting controller reports or every stick is
 * permanently off centre. */
static void pad_origin_reply(uint8_t out[10]) {
  out[0] = 0;
  out[1] = 0;
  out[2] = 128;
  out[3] = 128;
  out[4] = 128;
  out[5] = 128;
  out[6] = 0;
  out[7] = 0;
  out[8] = 0;
  out[9] = 0;
}

/* The bytes a channel's input registers hold between transfers, which is
 * where automatic polling leaves its answers and where the SDK's own pad
 * driver reads from. */
static void si_refresh_inputs(GcMachine *m) {
  unsigned channel;
  uint32_t status = 0;
  for (channel = 0; channel < GC_PAD_PORTS; channel++) {
    const unsigned shift = 24u - channel * 8u;
    uint32_t *high = io_slot(m, "si", 0x04u + channel * 0x0cu);
    uint32_t *low = io_slot(m, "si", 0x08u + channel * 0x0cu);
    const GcPad *pad = pad_of(m, channel);
    uint8_t reply[8];
    if (!high || !low) continue;
    if (!pad) {
      *high = 0;
      *low = 0;
      status |= (uint32_t)GC_SI_NO_RESPONSE << shift;
      continue;
    }
    pad_poll_reply(pad, reply);
    *high = ((uint32_t)reply[0] << 24) | ((uint32_t)reply[1] << 16) |
            ((uint32_t)reply[2] << 8) | reply[3];
    *low = ((uint32_t)reply[4] << 24) | ((uint32_t)reply[5] << 16) |
           ((uint32_t)reply[6] << 8) | reply[7];
    status |= (uint32_t)GC_SI_READ_STATUS << shift;
  }
  {
    uint32_t *slot = io_slot(m, "si", 0x38);
    if (slot) *slot = status;
  }
}

/* The interface's shared buffer, a byte at a time. It is described as
 * thirty-two words because that is how software reaches it, but a command
 * and its reply are bytes. */
static uint8_t si_buffer_read(GcMachine *m, unsigned offset) {
  const uint32_t *slot = io_slot(m, "si", 0x80u + (offset & ~3u));
  if (!slot) return 0;
  return (uint8_t)(*slot >> (8u * (3u - (offset & 3u))));
}

static void si_buffer_write(GcMachine *m, unsigned offset, uint8_t value) {
  uint32_t *slot = io_slot(m, "si", 0x80u + (offset & ~3u));
  const unsigned shift = 8u * (3u - (offset & 3u));
  if (!slot) return;
  *slot = (*slot & ~(0xffu << shift)) | ((uint32_t)value << shift);
}

/* A transfer on one channel: a command out of the buffer, a reply back
 * into it. */
static void si_transfer(GcMachine *m, uint32_t control) {
  const unsigned channel = (control >> 1) & 3u;
  const unsigned in_length = ((control >> 8) & 0x7fu) ? ((control >> 8) & 0x7fu) : 128u;
  const GcPad *pad = pad_of(m, channel);
  uint32_t *status = io_slot(m, "si", 0x38);
  uint32_t *slot = io_slot(m, "si", 0x34);
  uint8_t reply[16];
  unsigned length = 0, i;

  if (!slot) return;
  memset(reply, 0, sizeof(reply));

  if (pad) {
    switch (si_buffer_read(m, 0)) {
      case 0x00: /* what are you */
      case 0xff: /* the same question, and a reset */
        reply[0] = (uint8_t)((GC_SI_TYPE_STANDARD >> 24) & 0xffu);
        reply[1] = (uint8_t)((GC_SI_TYPE_STANDARD >> 16) & 0xffu);
        reply[2] = (uint8_t)((GC_SI_TYPE_STANDARD >> 8) & 0xffu);
        length = 3;
        break;
      case 0x40: /* the usual poll */
        pad_poll_reply(pad, reply);
        length = 8;
        break;
      case 0x41: /* where the sticks rest */
      case 0x42: /* and again, having been asked to recalibrate */
        pad_origin_reply(reply);
        length = 10;
        break;
      case 0x43: /* the long form, which adds the two analogue buttons */
        pad_poll_reply(pad, reply);
        length = 10;
        break;
      default:
        length = 0;
        break;
    }
  }

  if (!length) {
    /* Nothing there, or nothing this understands. The error is the answer:
     * a game that asks an empty port expects to be told so, and telling it
     * nothing at all leaves it waiting. */
    if (status) {
      *status = (*status & ~(0xffu << (24u - channel * 8u))) |
                ((uint32_t)GC_SI_NO_RESPONSE << (24u - channel * 8u));
    }
    /* Bit 27 is the communication error the transfer itself reports. */
    *slot = (control & ~1u) | 0x08000000u | 0x80000000u;
    gc_si_update_interrupt(m);
    return;
  }

  for (i = 0; i < in_length && i < sizeof(reply); i++) si_buffer_write(m, i, reply[i]);
  if (status) {
    *status = (*status & ~(0xffu << (24u - channel * 8u))) |
              ((uint32_t)GC_SI_READ_STATUS << (24u - channel * 8u));
  }
  /* Done, and complete: the start bit falls and the completion bit rises. */
  *slot = (control & ~1u & ~0x08000000u) | 0x80000000u;
  gc_si_update_interrupt(m);
}

static void si_control_write(GcMachine *m, uint32_t value) {
  uint32_t *slot = io_slot(m, "si", 0x34);
  if (!slot) return;
  /* The completion bit is written back to clear, like every other one. */
  *slot = (value & ~0x80000000u) | ((*slot & ~value) & 0x80000000u);
  if (value & 1u) si_transfer(m, value);
  else gc_si_update_interrupt(m);
}

static uint32_t si_status_read(GcMachine *m, uint32_t stored) {
  uint32_t *slot;
  (void)stored;
  si_refresh_inputs(m);
  slot = io_slot(m, "si", 0x38);
  return slot ? *slot : 0u;
}

/* The interface polls the ports by itself, at a rate a game sets, and
 * leaves the answers in the input registers. Doing it once a field is the
 * rate every game actually asks for, and it means the registers hold
 * something current whenever one is read - which is how the SDK's own pad
 * driver expects to find them. */
void gc_si_field_boundary(GcMachine *m) { si_refresh_inputs(m); }

/* Serial interface: the controller ports. */
static const GcIoReg si_regs[] = {
    REG(0x00, 4, "si_channel_0_output"), REG(0x04, 4, "si_channel_0_input_hi"),
    REG(0x08, 4, "si_channel_0_input_lo"), REG(0x0c, 4, "si_channel_1_output"),
    REG(0x10, 4, "si_channel_1_input_hi"), REG(0x14, 4, "si_channel_1_input_lo"),
    REG(0x18, 4, "si_channel_2_output"), REG(0x1c, 4, "si_channel_2_input_hi"),
    REG(0x20, 4, "si_channel_2_input_lo"), REG(0x24, 4, "si_channel_3_output"),
    REG(0x28, 4, "si_channel_3_input_hi"), REG(0x2c, 4, "si_channel_3_input_lo"),
    REG(0x30, 4, "si_poll"),
    REG_HOOK(0x34, 4, "si_communication_control", NULL, si_control_write),
    REG_HOOK(0x38, 4, "si_status", si_status_read, NULL),
    REG(0x3c, 4, "si_exi_clock_lock"),
    /* The 128-byte buffer a command and its reply go through. */
    REG(0x80, 4, "si_buffer_00"), REG(0x84, 4, "si_buffer_04"),
    REG(0x88, 4, "si_buffer_08"), REG(0x8c, 4, "si_buffer_0c"),
    REG(0x90, 4, "si_buffer_10"), REG(0x94, 4, "si_buffer_14"),
    REG(0x98, 4, "si_buffer_18"), REG(0x9c, 4, "si_buffer_1c"),
    REG(0xa0, 4, "si_buffer_20"), REG(0xa4, 4, "si_buffer_24"),
    REG(0xa8, 4, "si_buffer_28"), REG(0xac, 4, "si_buffer_2c"),
    REG(0xb0, 4, "si_buffer_30"), REG(0xb4, 4, "si_buffer_34"),
    REG(0xb8, 4, "si_buffer_38"), REG(0xbc, 4, "si_buffer_3c"),
};

/* External interface: memory cards, the SRAM the settings live in, and the
 * debug devices. */
static const GcIoReg exi_regs[] = {
    REG_HOOK(0x00, 4, "exi_0_status", exi_0_status_read, exi_0_status_write),
    REG(0x04, 4, "exi_0_dma_address"),
    REG(0x08, 4, "exi_0_dma_length"),
    REG_HOOK(0x0c, 4, "exi_0_control", NULL, exi_0_control),
    REG(0x10, 4, "exi_0_immediate"),
    REG_HOOK(0x14, 4, "exi_1_status", exi_1_status_read, exi_1_status_write),
    REG(0x18, 4, "exi_1_dma_address"), REG(0x1c, 4, "exi_1_dma_length"),
    REG_HOOK(0x20, 4, "exi_1_control", NULL, exi_1_control),
    REG(0x24, 4, "exi_1_immediate"),
    REG_HOOK(0x28, 4, "exi_2_status", exi_2_status_read, exi_2_status_write),
    REG(0x2c, 4, "exi_2_dma_address"), REG(0x30, 4, "exi_2_dma_length"),
    REG_HOOK(0x34, 4, "exi_2_control", NULL, exi_2_control),
    REG(0x38, 4, "exi_2_immediate"),
};

/* Audio interface: the streaming clock and its sample counter. */
static const GcIoReg ai_regs[] = {
    REG_HOOK_W1C(0x00, 4, "ai_control", NULL, ai_control_write, GC_AI_INTERRUPT),
    REG(0x04, 4, "ai_volume"),
    REG_HOOK(0x08, 4, "ai_sample_count", ai_sample_count_read, ai_sample_count_write),
    REG(0x0c, 4, "ai_interrupt_timing"),
};

#define BLOCK(name, base, size, regs)   {(name), (base), (size), (regs), (unsigned)(sizeof(regs) / sizeof((regs)[0])), 0}

static GcIoBlock io_blocks[] = {
    BLOCK("cp", 0x0000, 0x0080, cp_regs),   BLOCK("pe", 0x1000, 0x0100, pe_regs),
    BLOCK("vi", 0x2000, 0x0100, vi_regs),   BLOCK("pi", 0x3000, 0x0100, pi_regs),
    BLOCK("mi", 0x4000, 0x0080, mi_regs),   BLOCK("dsp", 0x5000, 0x0200, dsp_regs),
    BLOCK("di", 0x6000, 0x0040, di_regs),   BLOCK("si", 0x6400, 0x0100, si_regs),
    BLOCK("exi", 0x6800, 0x0040, exi_regs), BLOCK("ai", 0x6c00, 0x0020, ai_regs),
};

#define IO_BLOCK_COUNT (sizeof(io_blocks) / sizeof(io_blocks[0]))

/* Each block's registers occupy a run of slots in the machine's storage;
 * the run is laid out once, at reset, in table order. */
void gc_io_reset(GcMachine *m) {
  unsigned i, used = 0;
  memset(m->io, 0, sizeof(m->io));
  /* The cached slots belong to the layout that is about to be laid out
   * again. Marked missing rather than left at zero, because zero is a
   * perfectly good index into the storage and a stale one would quietly
   * name the wrong register instead of failing. */
  m->hot_slots_known = 0;
  m->ai_control_slot = m->ai_timing_slot = m->dsp_control_slot = GC_SLOT_MISSING;
  for (i = 0; i < IO_BLOCK_COUNT; i++) {
    io_blocks[i].storage_base = used;
    used += io_blocks[i].count;
  }
  /* A build that outgrows the storage array has to say so rather than
   * scribble past it. */
  if (used > GC_IO_STORAGE) {
    if (m->log) fprintf(m->log, "gc: the I/O register tables need %u slots\n", used);
    m->bad_access++;
  }
}


/* The storage slot behind a register, by block name and offset. Hooks use
 * it to reach the other registers of their own block. */
static uint32_t *io_slot(GcMachine *m, const char *block_name, uint32_t offset) {
  unsigned i, r;
  for (i = 0; i < IO_BLOCK_COUNT; i++) {
    if (strcmp(io_blocks[i].name, block_name) != 0) continue;
    for (r = 0; r < io_blocks[i].count; r++) {
      if (io_blocks[i].regs[r].offset == offset) return &m->io[io_blocks[i].storage_base + r];
    }
  }
  return NULL;
}

static uint32_t *exi_slot(GcMachine *m, uint32_t offset) { return io_slot(m, "exi", offset); }

static GcIoBlock *block_for(uint32_t offset) {
  unsigned i;
  for (i = 0; i < IO_BLOCK_COUNT; i++) {
    if (offset >= io_blocks[i].base && offset < io_blocks[i].base + io_blocks[i].size) {
      return &io_blocks[i];
    }
  }
  return NULL;
}

/* The register an access lands in, exactly or by covering part of it: the
 * guest may write the low half of a 32-bit register on its own, and the
 * hardware lets it. `within` comes back as the byte offset into the
 * register. */
static const GcIoReg *reg_for(const GcIoBlock *block, uint32_t offset, unsigned *index,
                              unsigned *within) {
  const uint32_t local = offset - block->base;
  unsigned i;
  for (i = 0; i < block->count; i++) {
    if (block->regs[i].offset == local) {
      *index = i;
      *within = 0;
      return &block->regs[i];
    }
  }
  for (i = 0; i < block->count; i++) {
    if (local > block->regs[i].offset &&
        local < (uint32_t)block->regs[i].offset + block->regs[i].width) {
      *index = i;
      *within = local - block->regs[i].offset;
      return &block->regs[i];
    }
  }
  return NULL;
}

static uint32_t io_read_one(GcMachine *m, uint32_t offset, unsigned size);
static void io_write_one(GcMachine *m, uint32_t offset, unsigned size, uint32_t value);

/* A register file is not addressed the way memory is: the guest may cover
 * two 16-bit registers with one 32-bit store, and each half has to reach
 * its own register and its own hook. */
uint32_t gc_io_read(GcMachine *m, uint32_t offset, unsigned size) {
  GcIoBlock *block;
  const GcIoReg *reg;
  unsigned index = 0, within = 0;
  /* Recompiled code lets the beam and the clocks fall behind between the
   * moments anything is due, so a register that reports them is brought up
   * to date first - and whatever this access changes is checked before the
   * next instruction. For the interpreter, which is never behind, both are
   * no-ops. */
  gc_machine_sync(m);
  m->cpu.next_event = 0;
  block = block_for(offset);
  if (block && (reg = reg_for(block, offset, &index, &within)) != NULL && size > reg->width) {
    uint32_t value = 0;
    unsigned done;
    for (done = 0; done < size; done += reg->width) {
      value = (value << (8 * reg->width)) | io_read_one(m, offset + done, reg->width);
    }
    return value;
  }
  return io_read_one(m, offset, size);
}

void gc_io_write(GcMachine *m, uint32_t offset, unsigned size, uint32_t value) {
  GcIoBlock *block;
  const GcIoReg *reg;
  unsigned index = 0, within = 0;
  gc_machine_sync(m); /* as for a read */
  m->cpu.next_event = 0;
  block = block_for(offset);
  if (block && (reg = reg_for(block, offset, &index, &within)) != NULL && size > reg->width) {
    const unsigned bits = 8u * reg->width;
    unsigned done;
    for (done = 0; done < size; done += reg->width) {
      const unsigned shift = 8u * (size - done - reg->width);
      io_write_one(m, offset + done, reg->width,
                   (value >> shift) & (bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u)));
    }
    return;
  }
  io_write_one(m, offset, size, value);
}

static uint32_t io_read_one(GcMachine *m, uint32_t offset, unsigned size) {
  GcIoBlock *block = block_for(offset);
  const GcIoReg *reg;
  unsigned index = 0, within = 0;
  uint32_t value;

  if (!block) {
    m->unknown_io_reads++;
    if (m->log && m->unknown_io_reads <= 16) {
      fprintf(m->log, "gc: read of unmapped I/O %08x (%u bytes), pc %08x\n",
              GC_PHYS_IO + offset, size, m->cpu.pc);
    }
    return 0;
  }
  reg = reg_for(block, offset, &index, &within);
  if (!reg) {
    m->unknown_io_reads++;
    if (m->log && m->unknown_io_reads <= 16) {
      fprintf(m->log, "gc: read of undescribed %s register +%03x (%u bytes), pc %08x\n",
              block->name, offset - block->base, size, m->cpu.pc);
    }
    return 0;
  }
  value = m->io[block->storage_base + index];
  if (reg->on_read) value = reg->on_read(m, value);
  value &= reg->read_mask;
  if (size < reg->width) {
    /* Part of a register: take the bytes the access covers. */
    const unsigned shift = 8u * (reg->width - within - size);
    const uint32_t mask = (size >= 4) ? 0xffffffffu : ((1u << (8u * size)) - 1u);
    value = (value >> shift) & mask;
  }
  if (m->trace_io && m->log) {
    fprintf(m->log, "gc: %s%s -> %08x\n", reg->name, within ? "(part)" : "", value);
  }
  return value;
}

static void io_write_one(GcMachine *m, uint32_t offset, unsigned size, uint32_t value) {
  GcIoBlock *block = block_for(offset);
  const GcIoReg *reg;
  unsigned index = 0, within = 0;

  if (!block) {
    m->unknown_io_writes++;
    if (m->log && m->unknown_io_writes <= 16) {
      fprintf(m->log, "gc: write of %08x to unmapped I/O %08x (%u bytes), pc %08x\n", value,
              GC_PHYS_IO + offset, size, m->cpu.pc);
    }
    return;
  }
  reg = reg_for(block, offset, &index, &within);
  if (!reg) {
    m->unknown_io_writes++;
    if (m->log && m->unknown_io_writes <= 16) {
      fprintf(m->log, "gc: write of %08x to undescribed %s register +%03x, pc %08x\n", value,
              block->name, offset - block->base, m->cpu.pc);
    }
    return;
  }
  if (m->trace_io && m->log) {
    fprintf(m->log, "gc: %s%s <- %08x\n", reg->name, within ? "(part)" : "", value);
  }
  {
    uint32_t *slot = &m->io[block->storage_base + index];
    uint32_t whole = value;
    if (size < reg->width) {
      /* Part of a register: merge the bytes the access covers into it. */
      const unsigned shift = 8u * (reg->width - within - size);
      const uint32_t mask = ((size >= 4) ? 0xffffffffu : ((1u << (8u * size)) - 1u)) << shift;
      whole = (*slot & ~mask) | ((value << shift) & mask);
    }
    *slot = (*slot & ~reg->write_mask) | (whole & reg->write_mask);
    /* A hook always sees the whole register, however narrow the access -
     * except the bits the store left alone (REG_HOOK_W1C's interrupt
     * status). For those it has to see what was *written*: they are still
     * set in the slot while an interrupt is pending, and a hook shown the
     * slot takes every pending interrupt for an acknowledgement. Shrek 2's
     * ARAM handler writes DSPCR with the DSP interrupt masked out, and a
     * hook that saw the slot cleared that interrupt anyway - the sound
     * task's mail was never read again. */
    if (reg->on_write) reg->on_write(m, (*slot & reg->write_mask) | (whole & ~reg->write_mask));
  }
}

/* ------------------------------------------------------------------ */

void gc_set_interrupt(GcMachine *m, unsigned cause, int raised) {
  const uint32_t bit = 1u << cause;
  if (raised) {
    m->pi_cause |= bit;
  } else {
    m->pi_cause &= ~bit;
  }
  /* A line that moved may be one the processor has to take before its next
   * instruction. */
  m->cpu.next_event = 0;
}

/* The register behind a storage slot, and its address: for a report that
 * has to name a register rather than an index. Null for a slot no register
 * uses. */
const char *gc_io_slot_name(unsigned slot, uint32_t *address) {
  unsigned i;
  for (i = 0; i < IO_BLOCK_COUNT; i++) {
    const GcIoBlock *block = &io_blocks[i];
    if (slot < block->storage_base || slot >= block->storage_base + block->count) continue;
    if (address) *address = 0xcc000000u + block->base + block->regs[slot - block->storage_base].offset;
    return block->regs[slot - block->storage_base].name;
  }
  return NULL;
}
