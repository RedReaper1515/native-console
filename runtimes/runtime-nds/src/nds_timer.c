/*
 * Timers 0-3 of each CPU (GBATEK, "DS Timers": the GBA's, clocked by the
 * 33.51 MHz system clock on both processors).
 *
 * A running timer is stored as (counter value, cycle it was captured) and
 * read lazily; its overflow is a scheduled event, so nothing ticks per
 * cycle. Count-up (cascade) timers advance when the previous timer
 * overflows. The prescalers divide one free-running clock, so a prescaled
 * timer ticks on multiples of 64, 256 or 1024 cycles, not counting from
 * its start.
 */
#include "nds.h"

static const uint32_t k_prescale_shift[4] = { 0, 6, 8, 10 };

static void overflow_signals(NdsMachine *m, int cpu, int index);

static inline int event_of(int cpu, int index)
{
    return (cpu == NDS_ARM9 ? NDS_EVENT_TIMER9_0 : NDS_EVENT_TIMER7_0) + index;
}

static inline uint32_t shift_of(const NdsTimer *t)
{
    return k_prescale_shift[t->control & 3u];
}

/* Value of a timer at `when`, wrapping through the reload value if it
 * overflowed on the way (inside an instruction, before the event ran). */
static uint16_t counter_at(const NdsTimer *t, uint64_t when)
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

static void schedule_overflow(NdsMachine *m, int cpu, int index)
{
    NdsTimer *t = &m->timer[cpu][index];
    if (!t->running) {
        nds_schedule(m, event_of(cpu, index), UINT64_MAX);
        return;
    }
    /* The counter moves at every multiple of the prescale period. */
    uint32_t s = shift_of(t);
    uint64_t ticks = 0x10000u - t->counter;
    nds_schedule(m, event_of(cpu, index), ((t->since >> s) + ticks) << s);
}

uint16_t nds_timer_read_counter(NdsMachine *m, int cpu, int index)
{
    return counter_at(&m->timer[cpu][index], nds_cpu_now(m, cpu));
}

void nds_timer_write_reload(NdsMachine *m, int cpu, int index, uint16_t value)
{
    m->timer[cpu][index].reload = value;
}

void nds_timer_write_control(NdsMachine *m, int cpu, int index, uint16_t value)
{
    NdsTimer *t = &m->timer[cpu][index];
    const uint64_t now = nds_cpu_now(m, cpu);
    bool was_enabled = (t->control & 0x80u) != 0;
    bool enabled = (value & 0x80u) != 0;

    /* Freeze the value so the change starts from it. */
    t->counter = counter_at(t, now);
    t->since = now;
    t->control = value;
    if (!was_enabled && enabled) t->counter = t->reload;
    t->running = enabled && !(index > 0 && (value & 0x04u));
    schedule_overflow(m, cpu, index);
}

static void count_up(NdsMachine *m, int cpu, int index);

/* What an overflow signals: the interrupt, and a count-up timer above. */
static void overflow_signals(NdsMachine *m, int cpu, int index)
{
    NdsTimer *t = &m->timer[cpu][index];
    if (t->control & 0x40u) nds_raise_irq(m, cpu, NDS_IRQ_TIMER0 << index);
    if (index < 3) {
        NdsTimer *next = &m->timer[cpu][index + 1];
        if ((next->control & 0x84u) == 0x84u) count_up(m, cpu, index + 1);
    }
}

static void overflow(NdsMachine *m, int cpu, int index)
{
    m->timer[cpu][index].counter = m->timer[cpu][index].reload;
    overflow_signals(m, cpu, index);
}

static void count_up(NdsMachine *m, int cpu, int index)
{
    NdsTimer *t = &m->timer[cpu][index];
    if (t->counter == 0xFFFFu) overflow(m, cpu, index);
    else t->counter++;
}

void nds_timer_overflow_event(NdsMachine *m, int cpu, int index)
{
    NdsTimer *t = &m->timer[cpu][index];
    uint64_t at = m->event_at[event_of(cpu, index)];
    overflow(m, cpu, index);
    t->since = at;
    schedule_overflow(m, cpu, index);
}
