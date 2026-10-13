/*
 * Link cable: the serial port in multi-player mode and the lockstep
 * protocol that connects machines in different processes (see gba_link.h
 * for the scheme).
 *
 * Multi-player mode (GBATEK "SIO Multi-Player Mode"): SIOCNT bits 12-13 = 2
 * with RCNT bit 15 clear. The parent (player 1, SI low) sets SIOCNT bit 7;
 * every connected GBA then sends its SIOMLT_SEND halfword in turn, and when
 * the transfer ends each one has all of them in SIOMULTI0-3 (0xFFFF for a
 * missing player), bit 7 clears, bits 4-5 hold its own player number, and
 * the serial interrupt fires if SIOCNT bit 14 asks for it. SIOCNT bit 2
 * reads the SI line (0 = parent, 1 = child), bit 3 the SD line (1 = all
 * GBAs ready, i.e. others are connected).
 *
 * Other serial modes (normal, UART, JOY bus, general purpose) keep the
 * plain register storage they have without a cable.
 *
 * Messages, all broadcast to the other players (little endian):
 *   'T' i64 time                      how far the sender's machine has run
 *   'X' u32 seq, i64 start, u32 cycles, u16 word, u8 players
 *                                     the parent started a transfer
 *   'W' u32 seq, u16 word             the sender's SIOMLT_SEND for it
 * Times are link time: the parent's CPU cycle count. A child learns its
 * offset from the parent's first 'T' after it joins.
 */
#include "gba_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* A child runs at most this far past the parent's last reported time. It
 * must stay below the shortest transfer (two players at 115200 bps, about
 * 6100 cycles), with room for the instruction or DMA that crosses it. */
#define LOOKAHEAD 4096
/* The parent runs at most this far past the slowest child. */
#define LEADER_WINDOW ((int64_t)GBA_CYCLES_PER_FRAME * 2)
/* Report progress at least this often while running. */
#define HEARTBEAT 16384
/* Look for players joining this often while alone. */
#define ALONE_POLL 65536
#define QUEUE 8
#define SEND_HISTORY 16

enum { STALL_BOUND = 1, STALL_DATA = 2 };

typedef struct Transfer {
    uint32_t seq;
    int64_t start, end;
    uint16_t words[GBA_LINK_MAX_PLAYERS];
    uint8_t have, need; /* players whose word arrived, players taking part */
    bool started;       /* reached `start` on this machine */
    bool joined;        /* this machine was in multi-player mode then */
} Transfer;

struct GbaLink {
    GbaMachine *m;
    GbaLinkIo io;
    int self;           /* player number - 1, -1 = not connected yet */
    uint8_t present;
    bool synced;        /* a child that knows the parent's clock */
    int64_t base;       /* link time = cycles - base */
    int64_t last_now;
    int64_t peer_time[GBA_LINK_MAX_PLAYERS];
    int64_t next_beat;
    int64_t last_sent;
    uint32_t seq;
    Transfer queue[QUEUE]; /* in start order; the parent has at most one */
    unsigned head, count;
    uint64_t send_cycle[SEND_HISTORY]; /* SIOMLT_SEND writes (machine cycles), a ring */
    uint16_t send_value[SEND_HISTORY];
    unsigned send_next, send_count;
    uint8_t id; /* multi-player ID shown in SIOCNT bits 4-5 after a transfer */
    uint8_t stall;
    int waiting_for;
    uint64_t transfers;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t clock_us(void)
{
#ifdef _WIN32
    /* GetTickCount64 only moves every 10-16 ms. */
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000u / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static int popcount4(uint8_t v)
{
    int n = 0;
    for (int i = 0; i < GBA_LINK_MAX_PLAYERS; i++) n += (v >> i) & 1;
    return n;
}

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static uint16_t io16(const GbaMachine *m, uint32_t off)
{
    return (uint16_t)(m->io[off] | (m->io[off + 1] << 8));
}

static void set_io16(GbaMachine *m, uint32_t off, uint16_t v)
{
    m->io[off] = (uint8_t)v;
    m->io[off + 1] = (uint8_t)(v >> 8);
}

static bool multi_mode(const GbaMachine *m)
{
    return !(io16(m, 0x134) & 0x8000u) && (io16(m, 0x128) & 0x3000u) == 0x2000u;
}

static int64_t link_now(const GbaLink *l)
{
    return (int64_t)l->m->cpu.cycles - l->base;
}

static int players(const GbaLink *l)
{
    return l->self < 0 ? 0 : popcount4(l->present);
}

uint32_t gba_link_transfer_cycles(int baud, int count)
{
    static const uint32_t rates[4] = { 9600, 38400, 57600, 115200 };
    if (count < 1) count = 1;
    /* Each GBA's halfword goes out framed by a start and a stop bit, with a
     * short gap before the next GBA's turn: about 20 bit times per player,
     * plus the parent's start signal. */
    return (uint32_t)((uint64_t)GBA_CPU_HZ * (20u * (uint32_t)count + 2u) / rates[baud & 3]);
}

void gba_link_game_id(const GbaMachine *m, char *out, size_t cap)
{
    char title[17] = "";
    if (m->rom && m->rom_size >= 0xC0) {
        for (int i = 0; i < 16; i++) {
            uint8_t c = m->rom[0xA0 + i];
            title[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
        }
        snprintf(out, cap, "%.16s-%02X%02X-%X", title, m->rom[0xBC], m->rom[0xBD], m->rom_size);
    } else {
        snprintf(out, cap, "unknown");
    }
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

static void send_time(GbaLink *l, int64_t now)
{
    if (l->self < 0 || (l->self > 0 && !l->synced)) return;
    uint8_t msg[9] = { 'T' };
    put_le(msg + 1, (uint64_t)now, 8);
    l->io.send(l->io.ctx, msg, sizeof msg);
    l->last_sent = now;
    l->next_beat = now + HEARTBEAT;
}

static void send_word(GbaLink *l, uint32_t seq, uint16_t word)
{
    uint8_t msg[7] = { 'W' };
    put_le(msg + 1, seq, 4);
    put_le(msg + 5, word, 2);
    l->io.send(l->io.ctx, msg, sizeof msg);
}

static Transfer *find_transfer(GbaLink *l, uint32_t seq)
{
    for (unsigned i = 0; i < l->count; i++) {
        Transfer *t = &l->queue[(l->head + i) % QUEUE];
        if (t->seq == seq) return t;
    }
    return NULL;
}

static Transfer *push_transfer(GbaLink *l)
{
    if (l->count == QUEUE) return NULL;
    Transfer *t = &l->queue[(l->head + l->count) % QUEUE];
    l->count++;
    memset(t, 0, sizeof *t);
    for (int i = 0; i < GBA_LINK_MAX_PLAYERS; i++) t->words[i] = 0xFFFFu;
    return t;
}

/* SIOMLT_SEND as it was at link time t. (Kept in machine cycles: a
 * child's link time only starts once it hears from the parent.) */
static uint16_t send_at(const GbaLink *l, int64_t t)
{
    int64_t local = t + l->base;
    uint16_t value = io16(l->m, 0x12A);
    for (unsigned i = 0; i < l->send_count; i++) {
        unsigned k = (l->send_next + SEND_HISTORY - 1 - i) % SEND_HISTORY;
        value = l->send_value[k];
        if ((int64_t)l->send_cycle[k] <= local) break;
    }
    return value;
}

static void record_send(GbaLink *l, uint16_t value)
{
    l->send_cycle[l->send_next] = l->m->cpu.cycles;
    l->send_value[l->send_next] = value;
    l->send_next = (l->send_next + 1) % SEND_HISTORY;
    if (l->send_count < SEND_HISTORY) l->send_count++;
}

/* ------------------------------------------------------------------ */
/* Transfers                                                           */
/* ------------------------------------------------------------------ */

/* A transfer reaches its start on a child: its registers show it, and its
 * word goes to the others. */
static void begin_transfer(GbaLink *l, Transfer *t)
{
    GbaMachine *m = l->m;
    t->started = true;
    t->joined = multi_mode(m);
    uint16_t word = 0xFFFFu;
    if (t->joined) {
        word = send_at(l, t->start);
        for (uint32_t off = 0x120; off < 0x128; off += 2) set_io16(m, off, 0xFFFFu);
    }
    if (l->self >= 0) {
        t->words[l->self] = word;
        t->have |= (uint8_t)(1u << l->self);
        send_word(l, t->seq, word);
    }
}

static void finish_transfer(GbaLink *l, Transfer *t)
{
    GbaMachine *m = l->m;
    if (t->joined) {
        for (int i = 0; i < GBA_LINK_MAX_PLAYERS; i++) {
            set_io16(m, 0x120 + 2u * (uint32_t)i, (t->need >> i) & 1u ? t->words[i] : 0xFFFFu);
        }
        l->id = (uint8_t)(l->self > 0 ? l->self : 0);
        if (io16(m, 0x128) & 0x4000u) gba_raise_irq(m, GBA_IRQ_SERIAL);
    }
    l->transfers++;
}

static void pop_transfer(GbaLink *l)
{
    l->head = (l->head + 1) % QUEUE;
    l->count--;
}

/* Advance transfers, report progress, and work out how far this machine
 * may run; stall it, or schedule the next time to look again. */
static void update(GbaLink *l)
{
    GbaMachine *m = l->m;
    int64_t now = link_now(l);
    l->last_now = now;
    l->stall = 0;
    l->waiting_for = -1;

    while (l->count) {
        Transfer *t = &l->queue[l->head];
        if (!t->started) {
            if (now < t->start) break;
            begin_transfer(l, t);
        }
        if (now < t->end) break;
        uint8_t need = (uint8_t)(t->need & (l->self < 0 ? 0 : l->present));
        uint8_t missing = (uint8_t)(need & ~t->have);
        if (missing) {
            l->stall |= STALL_DATA;
            for (int i = 0; i < GBA_LINK_MAX_PLAYERS; i++) {
                if (missing & (1u << i)) {
                    l->waiting_for = i;
                    break;
                }
            }
            break;
        }
        finish_transfer(l, t);
        pop_transfer(l);
    }

    int n = players(l);
    if (n >= 2 && now >= l->next_beat) send_time(l, now);

    int64_t bound = INT64_MAX;
    int bound_player = -1;
    if (n >= 2) {
        if (l->self == 0) {
            for (int i = 1; i < GBA_LINK_MAX_PLAYERS; i++) {
                if ((l->present & (1u << i)) && l->peer_time[i] + LEADER_WINDOW < bound) {
                    bound = l->peer_time[i] + LEADER_WINDOW;
                    bound_player = i;
                }
            }
        } else if (l->present & 1u) {
            bound = l->synced ? l->peer_time[0] + LOOKAHEAD : now;
            bound_player = 0;
        }
    }
    if (now >= bound) {
        l->stall |= STALL_BOUND;
        if (l->waiting_for < 0) l->waiting_for = bound_player;
    }

    if (l->stall) {
        if (l->last_sent != now) send_time(l, now);
        m->attention |= GBA_ATTENTION_STALL;
        gba_schedule_sio(m, UINT64_MAX);
        return;
    }
    m->attention = (uint8_t)(m->attention & ~GBA_ATTENTION_STALL);
    int64_t next = bound;
    if (n >= 2 && l->next_beat < next) next = l->next_beat;
    if (n < 2 && now + ALONE_POLL < next) next = now + ALONE_POLL;
    if (l->count) {
        const Transfer *t = &l->queue[l->head];
        int64_t due = t->started ? t->end : t->start;
        if (due < next) next = due;
    }
    gba_schedule_sio(m, (uint64_t)(next + l->base > 0 ? next + l->base : 0));
}

/* The parent sets SIOCNT bit 7. */
static void start_transfer(GbaLink *l)
{
    GbaMachine *m = l->m;
    if (l->count) return; /* busy */
    Transfer *t = push_transfer(l);
    if (!t) return;
    int n = players(l) > 0 ? players(l) : 1;
    int64_t now = link_now(l);
    t->seq = ++l->seq;
    t->start = now;
    t->end = now + gba_link_transfer_cycles(io16(m, 0x128) & 3u, n);
    t->need = (uint8_t)(l->present | 1u);
    t->started = t->joined = true;
    t->words[0] = io16(m, 0x12A);
    t->have = 1u;
    for (uint32_t off = 0x120; off < 0x128; off += 2) set_io16(m, off, 0xFFFFu);
    if (n >= 2) {
        uint8_t msg[20] = { 'X' };
        put_le(msg + 1, t->seq, 4);
        put_le(msg + 5, (uint64_t)t->start, 8);
        put_le(msg + 13, (uint64_t)(t->end - t->start), 4);
        put_le(msg + 17, t->words[0], 2);
        msg[19] = t->need;
        l->io.send(l->io.ctx, msg, sizeof msg);
        /* This is also how far the parent got. */
        l->last_sent = now;
    }
    update(l);
}

/* ------------------------------------------------------------------ */
/* Events from the transport                                           */
/* ------------------------------------------------------------------ */

static void handle_roster(GbaLink *l, const GbaLinkEvent *ev)
{
    uint8_t before = l->self < 0 ? 0 : l->present;
    l->self = ev->self;
    l->present = ev->self < 0 ? 0 : (uint8_t)(ev->present | (1u << ev->self));
    if (l->self <= 0 || !(l->present & 1u)) {
        /* Not connected to a parent (any more): forget its clock and what
         * it had started. */
        if (l->self != 0) {
            l->synced = false;
            l->count = 0;
        }
    }
    if (l->self == 0) {
        int64_t now = link_now(l);
        for (int i = 1; i < GBA_LINK_MAX_PLAYERS; i++) {
            if ((l->present & (1u << i)) && !(before & (1u << i))) l->peer_time[i] = now;
        }
        /* A child that just joined sets its clock from this. */
        if (l->present & ~before) send_time(l, now);
    }
}

static void handle_message(GbaLink *l, const GbaLinkEvent *ev)
{
    int from = ev->from;
    if (from < 0 || from >= GBA_LINK_MAX_PLAYERS || ev->len < 1) return;
    const uint8_t *d = ev->data;
    switch (d[0]) {
    case 'T': {
        if (ev->len < 9) return;
        int64_t t = (int64_t)get_le(d + 1, 8);
        if (l->self > 0 && from == 0 && !l->synced) {
            l->base = (int64_t)l->m->cpu.cycles - t;
            l->synced = true;
            l->last_sent = INT64_MIN;
            l->next_beat = t;
            l->peer_time[0] = t;
        } else if (t > l->peer_time[from]) {
            l->peer_time[from] = t;
        }
        break;
    }
    case 'X': {
        if (ev->len < 20 || from != 0 || l->self <= 0 || !l->synced) return;
        Transfer *t = push_transfer(l);
        if (!t) return;
        t->seq = (uint32_t)get_le(d + 1, 4);
        t->start = (int64_t)get_le(d + 5, 8);
        t->end = t->start + (int64_t)get_le(d + 13, 4);
        t->words[0] = (uint16_t)get_le(d + 17, 2);
        t->need = d[19];
        t->have = 1u;
        if (t->start > l->peer_time[0]) l->peer_time[0] = t->start;
        break;
    }
    case 'W': {
        if (ev->len < 7) return;
        Transfer *t = find_transfer(l, (uint32_t)get_le(d + 1, 4));
        if (!t) return;
        t->words[from] = (uint16_t)get_le(d + 5, 2);
        t->have |= (uint8_t)(1u << from);
        break;
    }
    default:
        break;
    }
}

static void handle(GbaLink *l, const GbaLinkEvent *ev)
{
    if (ev->kind == GBA_LINK_EVENT_ROSTER) handle_roster(l, ev);
    else if (ev->kind == GBA_LINK_EVENT_MESSAGE) handle_message(l, ev);
}

static int pump(GbaLink *l)
{
    int n = 0;
    GbaLinkEvent ev;
    while (l->io.poll(l->io.ctx, 0, &ev) > 0) {
        handle(l, &ev);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Machine side                                                        */
/* ------------------------------------------------------------------ */

uint16_t gba_sio_read_cnt(GbaMachine *m)
{
    GbaLink *l = m->link;
    uint16_t cnt = io16(m, 0x128);
    if (!multi_mode(m)) return cnt & 0x7FFFu;
    uint16_t v = (uint16_t)(cnt & 0x7F03u);
    if (l->self != 0) v |= 0x0004u;          /* SI: child */
    if (players(l) >= 2) v |= 0x0008u;       /* SD: all GBAs ready */
    v |= (uint16_t)((l->id & 3u) << 4);
    if (l->count && l->queue[l->head].started && l->queue[l->head].joined) v |= 0x0080u;
    return v;
}

void gba_sio_write(GbaMachine *m, uint32_t off, uint16_t value, uint16_t byte_mask)
{
    GbaLink *l = m->link;
    if (off == 0x12A) {
        set_io16(m, off, (uint16_t)((io16(m, off) & ~byte_mask) | (value & byte_mask)));
        record_send(l, io16(m, off));
        return;
    }
    uint16_t mask = (uint16_t)(0x7FFFu & byte_mask);
    set_io16(m, off, (uint16_t)((io16(m, off) & ~mask) | (value & mask)));
    if ((byte_mask & 0x00FFu) && (value & 0x0080u) && multi_mode(m) && l->self == 0) start_transfer(l);
}

void gba_sio_event(GbaMachine *m)
{
    GbaLink *l = m->link;
    if (!l) return;
    pump(l);
    update(l);
}

void gba_sio_state_loaded(GbaMachine *m)
{
    GbaLink *l = m->link;
    /* Link time carries on from where it was; the machine's clock moved. */
    l->base = (int64_t)m->cpu.cycles - l->last_now;
    update(l);
}

/* ------------------------------------------------------------------ */
/* Frontend side                                                       */
/* ------------------------------------------------------------------ */

GbaLink *gba_link_attach(GbaMachine *m, GbaLinkIo io, int self)
{
    if (m->link) gba_link_detach(m);
    GbaLink *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->m = m;
    l->io = io;
    l->self = self;
    l->present = self >= 0 ? (uint8_t)(1u << self) : 0;
    l->base = 0;
    l->last_now = link_now(l);
    l->last_sent = INT64_MIN;
    l->waiting_for = -1;
    m->link = l;
    record_send(l, io16(m, 0x12A));
    update(l);
    return l;
}

void gba_link_detach(GbaMachine *m)
{
    GbaLink *l = m->link;
    if (!l) return;
    m->link = NULL;
    m->attention = (uint8_t)(m->attention & ~GBA_ATTENTION_STALL);
    gba_schedule_sio(m, UINT64_MAX);
    free(l);
}

bool gba_link_wait(GbaLink *l, int timeout_ms)
{
    pump(l);
    update(l);
    if (!l->stall) return true;
    uint64_t deadline = clock_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    for (;;) {
        uint64_t t = clock_us();
        int remaining = t >= deadline ? 0 : (int)((deadline - t + 999) / 1000);
        GbaLinkEvent ev;
        if (l->io.poll(l->io.ctx, remaining, &ev) > 0) {
            handle(l, &ev);
            pump(l);
            update(l);
            if (!l->stall) return true;
        } else if (remaining == 0) {
            return false;
        }
    }
}

void gba_link_idle(GbaLink *l, int timeout_ms)
{
    GbaLinkEvent ev;
    if (l->io.poll(l->io.ctx, timeout_ms, &ev) > 0) {
        handle(l, &ev);
        pump(l);
    }
    update(l);
}

void gba_link_frame_done(GbaLink *l)
{
    pump(l);
    int64_t now = link_now(l);
    if (players(l) >= 2 && l->last_sent != now) send_time(l, now);
    update(l);
}

bool gba_link_follows(const GbaLink *l)
{
    return l && l->self > 0 && l->synced && (l->present & 1u);
}

void gba_link_status(const GbaLink *l, GbaLinkStatus *out)
{
    memset(out, 0, sizeof *out);
    out->self = l->self;
    out->players = players(l);
    out->present = l->self < 0 ? 0 : l->present;
    out->stalled = l->stall != 0;
    out->waiting_for = l->stall ? l->waiting_for : -1;
    out->transfers = l->transfers;
    out->time = link_now(l);
}
