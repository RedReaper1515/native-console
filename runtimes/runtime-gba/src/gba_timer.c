/*
 * Timers 0-3.
 *
 * A running timer is stored as (counter value, cycle it was captured) and
 * read lazily; its overflow is a scheduled event, so nothing ticks per
 * cycle. Count-up (cascade) timers advance when the previous timer
 * overflows.
 *
 * Timing, as the Alyosha timer and prefetcher tests pin it down (GBAHawk
 * implements the same): a timer starts counting 2 cycles after the write
 * that enables it, and one being disabled still ticks in the cycle after
 * the write. The prescalers divide one free-running clock, so a prescaled
 * timer ticks on multiples of 64, 256 or 1024 cycles, not counting from
 * its start.
 */
#include "gba.h"

static const uint32_t k_prescale_shift[4] = { 0, 6, 8, 10 };

static void overflow_signals(GbaMachine *m, int index);

static inline uint32_t shift_of(const GbaTimer *t)
{
    return k_prescale_shift[t->control & 3u];
}

/* Value of a timer at `when`, wrapping through the reload value if it
 * overflowed on the way (inside an instruction, before the event ran). */
static uint16_t counter_at(const GbaTimer *t, uint64_t when)
{
    if (!t->running || when <= t->since) return t->counter;
    uint32_t s = shift_of(t);
    uint64_t value = (uint64_t)t->counter + ((when >> s) - (t->since >> s));
    if (value > 0xFFFFu) {
        uint64_t period = 0x10000u - t->reload;
        value = t->reload + (value - 0x10000u) % period;
    }
    return (uint16_t)value;
}

static void schedule_overflow(GbaMachine *m, int index)
{
    GbaTimer *t = &m->timer[index];
    int event = GBA_EVENT_TIMER0 + index;
    if (!t->running) {
        gba_schedule(m, event, UINT64_MAX);
        return;
    }
    /* The counter moves at every multiple of the prescale period. */
    uint32_t s = shift_of(t);
    uint64_t ticks = 0x10000u - t->counter;
    gba_schedule(m, event, ((t->since >> s) + ticks) << s);
}

/* Freeze the value at `when` so control changes start from it. */
static void capture(GbaMachine *m, int index, uint64_t when)
{
    GbaTimer *t = &m->timer[index];
    t->counter = counter_at(t, when);
    t->since = when;
}

uint16_t gba_timer_read_counter(GbaMachine *m, int index)
{
    return counter_at(&m->timer[index], m->cpu.cycles);
}

void gba_timer_write_reload(GbaMachine *m, int index, uint16_t value)
{
    m->timer[index].reload = value;
}

void gba_timer_write_control(GbaMachine *m, int index, uint16_t value)
{
    GbaTimer *t = &m->timer[index];
    /* Overflows that are already due happen first. */
    if (m->cpu.cycles >= m->next_event) gba_process_events(m);
    const uint64_t now = m->cpu.cycles;
    bool was_enabled = (t->control & 0x80u) != 0;
    bool enabled = (value & 0x80u) != 0;

    if (was_enabled && !enabled && t->running) {
        /* A timer being stopped ticks once more, in the next cycle. */
        uint64_t stop = now + 1;
        if (m->event_at[GBA_EVENT_TIMER0 + index] <= stop) gba_timer_overflow_event(m, index);
        capture(m, index, stop);
    } else {
        capture(m, index, now);
    }
    bool glitch_tick = false;
    if (!was_enabled && enabled) {
        /* Enabled while the counter sits at 0xFFFF, it ticks once more
         * before the reload value is loaded, in the cycle after the write
         * (if the prescaler ticks then), and so overflows. */
        uint32_t s = k_prescale_shift[value & 3u];
        glitch_tick = t->counter == 0xFFFFu && (index == 0 || !(value & 0x04u)) && ((now + 2) & ((1u << s) - 1u)) == 0;
    }
    t->control = value;
    if (!was_enabled && enabled) {
        t->counter = t->reload;
        t->since = now + 2;
    }
    t->running = enabled && !(index > 0 && (value & 0x04u));
    schedule_overflow(m, index);
    if (glitch_tick) overflow_signals(m, index);
}

static void count_up(GbaMachine *m, int index);

/* What an overflow signals: the interrupt, the sound FIFOs, a count-up timer. */
static void overflow_signals(GbaMachine *m, int index)
{
    GbaTimer *t = &m->timer[index];
    if (t->control & 0x40u) gba_raise_irq(m, (uint16_t)(GBA_IRQ_TIMER0 << index));
    if (index < 2) gba_apu_timer_overflow(m, index);
    if (index < 3) {
        GbaTimer *next = &m->timer[index + 1];
        if ((next->control & 0x84u) == 0x84u) count_up(m, index + 1);
    }
}

static void overflow(GbaMachine *m, int index)
{
    m->timer[index].counter = m->timer[index].reload;
    overflow_signals(m, index);
}

static void count_up(GbaMachine *m, int index)
{
    GbaTimer *t = &m->timer[index];
    if (t->counter == 0xFFFFu) overflow(m, index);
    else t->counter++;
}

void gba_timer_overflow_event(GbaMachine *m, int index)
{
    GbaTimer *t = &m->timer[index];
    uint64_t at = m->event_at[GBA_EVENT_TIMER0 + index];
    overflow(m, index);
    t->since = at;
    schedule_overflow(m, index);
}
