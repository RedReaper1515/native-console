/*
 * Game Boy link cable: the serial port between two machines in different
 * processes, in symmetric lockstep (see gbrt_link.h for the scheme).
 *
 * Serial port (Pan Docs "Serial Data Transfer"): SB (FF01) holds the byte
 * to send and receives the other side's; SC (FF02) bit 7 starts or waits
 * for a transfer, bit 0 selects the internal clock (this side drives it),
 * CGB bit 1 the fast clock. A side waiting with the external clock does
 * nothing until the other drives the clock. When the eight bits are done,
 * bit 7 clears and the serial interrupt (IF bit 3) is requested on each
 * side that took part.
 *
 * Messages to the other player (little endian):
 *   'C' u32 lookahead, u32 min_duration   the host's timing (on joining)
 *   'T' i64 time                          how far the sender has run
 *   'X' u32 seq, i64 start, u32 cycles, u8 byte
 *                                         the sender drove the clock
 *   'W' u32 seq, u8 byte, u8 joined       the reply: SB at the start, and
 *                                         whether it was waiting for it
 * Times are link time in system cycles, the host's count; the joiner takes
 * its offset from the host's first 'T'.
 */
#include "gbrt_link.h"
#include "gbrt_link_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* Both machines on one computer: messages take microseconds, so a short
 * lookahead keeps every normal-speed transfer at hardware timing. Across a
 * network the window is longer, so latency stalls less, and transfers are
 * stretched to match. MIN_DURATION must exceed LOOKAHEAD with room for the
 * instruction or DMA that crosses the bound. */
#define LOCAL_LOOKAHEAD 1536
#define LOCAL_MIN_DURATION 2048
#define NET_LOOKAHEAD 6144
#define NET_MIN_DURATION 8192
#define ALONE_POLL 65536
#define QUEUE 4
#define HISTORY 16

enum { STALL_BOUND = 1, STALL_DATA = 2 };

typedef struct Incoming {
    uint32_t seq;
    int64_t start, end;
    uint8_t byte;   /* the other side's SB */
    bool started;
    bool joined;    /* this side was waiting with the external clock at the start */
} Incoming;

struct GBLink {
    GBContext* ctx;
    GBLinkIo io;
    GBNet* net; /* owned when started by gbrt_link_host / join */
    int self;
    uint8_t present;
    bool synced;
    int64_t base;
    int64_t lookahead, min_duration;
    int64_t peer_time[GB_LINK_MAX_PLAYERS];
    int64_t next_beat, last_sent;
    uint32_t seq;
    Incoming queue[QUEUE];
    unsigned head, count;
    /* This side's own transfer (internal clock). */
    bool out_active, out_have, out_joined;
    uint32_t out_seq;
    int64_t out_start, out_end;
    uint8_t out_reply;
    /* SB and SC after each write, a ring in link_clock cycles. */
    uint64_t hist_cycle[HISTORY];
    uint8_t hist_sb[HISTORY], hist_sc[HISTORY];
    unsigned hist_next, hist_count;
    uint8_t stall;
    uint64_t transfers;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t clock_us(void)
{
#ifdef _WIN32
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

static void put_le(uint8_t* p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t* p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int64_t link_now(const GBLink* l)
{
    return (int64_t)l->ctx->link_clock - l->base;
}

static int players(const GBLink* l)
{
    if (l->self < 0) return 0;
    int n = 0;
    for (int i = 0; i < GB_LINK_MAX_PLAYERS; i++) n += (l->present >> i) & 1;
    return n;
}

static void record(GBLink* l)
{
    l->hist_cycle[l->hist_next] = l->ctx->link_clock;
    l->hist_sb[l->hist_next] = l->ctx->io[0x01];
    l->hist_sc[l->hist_next] = l->ctx->io[0x02];
    l->hist_next = (l->hist_next + 1) % HISTORY;
    if (l->hist_count < HISTORY) l->hist_count++;
}

/* SB and SC as they were at link time t. */
static void registers_at(const GBLink* l, int64_t t, uint8_t* sb, uint8_t* sc)
{
    int64_t local = t + l->base;
    *sb = l->ctx->io[0x01];
    *sc = l->ctx->io[0x02];
    for (unsigned i = 0; i < l->hist_count; i++) {
        unsigned k = (l->hist_next + HISTORY - 1 - i) % HISTORY;
        *sb = l->hist_sb[k];
        *sc = l->hist_sc[k];
        if ((int64_t)l->hist_cycle[k] <= local) break;
    }
}

/* The byte's time on the wire, in system cycles, at least min_duration. */
static int64_t transfer_duration(const GBLink* l)
{
    const GBContext* ctx = l->ctx;
    const bool cgb_mode = ctx->config.model == GB_MODEL_CGB && !ctx->config.cgb_compatibility_mode;
    const bool fast = cgb_mode && (ctx->io[0x02] & 0x02);
    int64_t d = fast ? 128 : 4096;
    if (ctx->cgb_double_speed) d /= 2;
    return d > l->min_duration ? d : l->min_duration;
}

static void request_serial_irq(GBContext* ctx)
{
    ctx->io[0x02] &= (uint8_t)~0x80;
    ctx->io[0x0F] |= 0x08;
    ctx->stopped = 1; /* let the dispatcher see the interrupt */
}

void gbrt_link_game_id(const GBContext* ctx, char* out, size_t cap)
{
    if (!ctx->rom || ctx->rom_size < 0x150) {
        snprintf(out, cap, "unknown");
        return;
    }
    char title[17];
    for (int i = 0; i < 16; i++) {
        uint8_t c = ctx->rom[0x134 + i];
        title[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
    }
    title[16] = '\0';
    snprintf(out, cap, "GB-%.16s-%02X%02X%02X", title, ctx->rom[0x14D], ctx->rom[0x14E], ctx->rom[0x14F]);
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

static void send_time(GBLink* l, int64_t now)
{
    if (l->self < 0 || (l->self > 0 && !l->synced)) return;
    uint8_t msg[9] = { 'T' };
    put_le(msg + 1, (uint64_t)now, 8);
    l->io.send(l->io.ctx, msg, sizeof msg);
    l->last_sent = now;
    l->next_beat = now + l->lookahead / 2;
}

static void send_config(GBLink* l)
{
    uint8_t msg[9] = { 'C' };
    put_le(msg + 1, (uint64_t)l->lookahead, 4);
    put_le(msg + 5, (uint64_t)l->min_duration, 4);
    l->io.send(l->io.ctx, msg, sizeof msg);
}

/* ------------------------------------------------------------------ */
/* Transfers                                                           */
/* ------------------------------------------------------------------ */

/* The other side's transfer reaches its start here: reply with SB. */
static void begin_incoming(GBLink* l, Incoming* in)
{
    uint8_t sb, sc;
    registers_at(l, in->start, &sb, &sc);
    in->started = true;
    /* Waiting with the external clock, and not driving a clock of our own. */
    in->joined = (sc & 0x81) == 0x80 && !(l->out_active && l->out_start <= in->start);
    uint8_t msg[7] = { 'W' };
    put_le(msg + 1, in->seq, 4);
    msg[5] = in->joined ? sb : 0xFF;
    msg[6] = in->joined ? 1 : 0;
    l->io.send(l->io.ctx, msg, sizeof msg);
}

static void finish_incoming(GBLink* l, Incoming* in)
{
    GBContext* ctx = l->ctx;
    if (in->joined && (ctx->io[0x02] & 0x81) == 0x80) {
        ctx->io[0x01] = in->byte;
        request_serial_irq(ctx);
        record(l);
    }
    l->transfers++;
}

static void finish_outgoing(GBLink* l)
{
    GBContext* ctx = l->ctx;
    l->out_active = false;
    if (!(ctx->io[0x02] & 0x80)) return; /* cancelled */
    uint8_t outgoing = ctx->io[0x01];
    ctx->io[0x01] = l->out_joined ? l->out_reply : 0xFF;
    request_serial_irq(ctx);
    record(l);
    l->transfers++;
    if (ctx->callbacks.on_serial_byte) ctx->callbacks.on_serial_byte(ctx, outgoing);
}

/* Advance transfers, report progress, and work out how far this machine
 * may run: stall it, or schedule the next look. */
static void update(GBLink* l)
{
    GBContext* ctx = l->ctx;
    const int64_t now = link_now(l);
    l->stall = 0;

    while (l->count) {
        Incoming* in = &l->queue[l->head];
        if (!in->started) {
            if (now < in->start) break;
            begin_incoming(l, in);
        }
        if (now < in->end) break;
        finish_incoming(l, in);
        l->head = (l->head + 1) % QUEUE;
        l->count--;
    }
    if (l->out_active && now >= l->out_end) {
        if (l->out_have) finish_outgoing(l);
        else l->stall |= STALL_DATA;
    }

    const int n = players(l);
    if (n >= 2 && now >= l->next_beat) send_time(l, now);

    int64_t bound = INT64_MAX;
    if (n >= 2) {
        const int other = l->self == 0 ? 1 : 0;
        if (l->self > 0 && !l->synced) bound = now;
        else bound = l->peer_time[other] + l->lookahead;
    }
    if (now >= bound) l->stall |= STALL_BOUND;

    if (l->stall) {
        if (l->last_sent != now) send_time(l, now);
        ctx->stopped = 1;
        ctx->link_next = UINT64_MAX;
        return;
    }
    int64_t next = bound;
    if (n >= 2 && l->next_beat < next) next = l->next_beat;
    if (n < 2 && now + ALONE_POLL < next) next = now + ALONE_POLL;
    if (l->count) {
        const Incoming* in = &l->queue[l->head];
        int64_t due = in->started ? in->end : in->start;
        if (due < next) next = due;
    }
    if (l->out_active && l->out_end < next) next = l->out_end;
    ctx->link_next = (uint64_t)(next + l->base > 0 ? next + l->base : 0);
}

static void start_outgoing(GBLink* l)
{
    GBContext* ctx = l->ctx;
    const int64_t now = link_now(l);
    l->out_active = true;
    l->out_seq = ++l->seq;
    l->out_start = now;
    l->out_end = now + transfer_duration(l);
    if (players(l) < 2) {
        /* No one on the other end: the bits read back as 1s. */
        l->out_have = true;
        l->out_joined = false;
        return;
    }
    l->out_have = false;
    uint8_t msg[18] = { 'X' };
    put_le(msg + 1, l->out_seq, 4);
    put_le(msg + 5, (uint64_t)now, 8);
    put_le(msg + 13, (uint64_t)(l->out_end - now), 4);
    msg[17] = ctx->io[0x01];
    l->io.send(l->io.ctx, msg, sizeof msg);
    l->last_sent = now; /* this also tells the other side how far we got */
}

/* ------------------------------------------------------------------ */
/* Events from the transport                                           */
/* ------------------------------------------------------------------ */

static void handle(GBLink* l, const GBLinkEvent* ev)
{
    if (ev->kind == GB_LINK_EVENT_ROSTER) {
        const uint8_t before = l->self < 0 ? 0 : l->present;
        l->self = ev->self;
        l->present = ev->self < 0 ? 0 : (uint8_t)(ev->present | (1u << ev->self));
        if (players(l) < 2) {
            /* The other side is gone: what it started never ends; our own
             * transfer finds nobody. */
            l->count = 0;
            if (l->out_active && !l->out_have) {
                l->out_have = true;
                l->out_joined = false;
            }
            if (l->self != 0) l->synced = false;
        }
        if (l->self == 0 && (l->present & ~before & 2u)) {
            l->peer_time[1] = link_now(l);
            send_config(l);
            send_time(l, link_now(l));
        }
        return;
    }
    if (ev->kind != GB_LINK_EVENT_MESSAGE || ev->len < 1) return;
    const int from = ev->from;
    if (from < 0 || from >= GB_LINK_MAX_PLAYERS || from == l->self) return;
    const uint8_t* d = ev->data;
    switch (d[0]) {
    case 'C':
        if (ev->len >= 9 && l->self > 0) {
            l->lookahead = (int64_t)get_le(d + 1, 4);
            l->min_duration = (int64_t)get_le(d + 5, 4);
        }
        break;
    case 'T': {
        if (ev->len < 9) return;
        const int64_t t = (int64_t)get_le(d + 1, 8);
        if (l->self > 0 && from == 0 && !l->synced) {
            l->base = (int64_t)l->ctx->link_clock - t;
            l->synced = true;
            l->last_sent = INT64_MIN;
            l->next_beat = t;
        }
        if (t > l->peer_time[from]) l->peer_time[from] = t;
        break;
    }
    case 'X': {
        if (ev->len < 18 || (l->self > 0 && !l->synced) || l->count == QUEUE) return;
        Incoming* in = &l->queue[(l->head + l->count) % QUEUE];
        memset(in, 0, sizeof *in);
        in->seq = (uint32_t)get_le(d + 1, 4);
        in->start = (int64_t)get_le(d + 5, 8);
        in->end = in->start + (int64_t)get_le(d + 13, 4);
        in->byte = d[17];
        l->count++;
        if (in->start > l->peer_time[from]) l->peer_time[from] = in->start;
        break;
    }
    case 'W':
        if (ev->len >= 7 && l->out_active && (uint32_t)get_le(d + 1, 4) == l->out_seq) {
            l->out_reply = d[5];
            l->out_joined = d[6] != 0;
            l->out_have = true;
        }
        break;
    default:
        break;
    }
}

static void pump(GBLink* l)
{
    GBLinkEvent ev;
    while (l->io.poll(l->io.ctx, 0, &ev) > 0) handle(l, &ev);
}

/* ------------------------------------------------------------------ */
/* Runtime hooks                                                       */
/* ------------------------------------------------------------------ */

void gbrt_link_event(GBContext* ctx)
{
    GBLink* l = ctx->link;
    if (!l) return;
    pump(l);
    update(l);
}

void gbrt_link_serial_written(GBContext* ctx, uint16_t addr)
{
    GBLink* l = ctx->link;
    record(l);
    if (addr != 0xFF02) return;
    const uint8_t sc = ctx->io[0x02];
    if ((sc & 0x81) == 0x81) {
        if (!l->out_active) {
            start_outgoing(l);
            update(l);
        }
    } else if (l->out_active && !(sc & 0x01)) {
        l->out_active = false; /* the game gave up driving the clock */
        update(l);
    }
}

uint32_t gbrt_link_cycles_until_event(const GBContext* ctx)
{
    if (!ctx->link) return UINT32_MAX;
    if (ctx->link_next <= ctx->link_clock) return 0;
    uint64_t room = ctx->link_next - ctx->link_clock;
    if (ctx->cgb_double_speed) room *= 2;
    return room > UINT32_MAX ? UINT32_MAX : (uint32_t)room;
}

bool gbrt_link_stalled(const GBContext* ctx)
{
    return ctx->link && ctx->link->stall;
}

/* ------------------------------------------------------------------ */
/* Frontend side                                                       */
/* ------------------------------------------------------------------ */

GBLink* gbrt_link_attach(GBContext* ctx, GBLinkIo io, int self, bool local)
{
    gbrt_link_detach(ctx);
    GBLink* l = (GBLink*)calloc(1, sizeof *l);
    if (!l) return NULL;
    l->ctx = ctx;
    l->io = io;
    l->self = self;
    l->present = self >= 0 ? (uint8_t)(1u << self) : 0;
    l->lookahead = local ? LOCAL_LOOKAHEAD : NET_LOOKAHEAD;
    l->min_duration = local ? LOCAL_MIN_DURATION : NET_MIN_DURATION;
    l->last_sent = INT64_MIN;
    ctx->link = l;
    record(l);
    /* The cable takes over from the no-cable stand-in, including a transfer
     * the game already started. */
    if (ctx->serial_transfer.active) {
        ctx->serial_transfer.active = 0;
        start_outgoing(l);
    }
    update(l);
    return l;
}

void gbrt_link_detach(GBContext* ctx)
{
    GBLink* l = ctx->link;
    if (!l) return;
    ctx->link = NULL;
    ctx->link_next = UINT64_MAX;
    /* A transfer this side was driving finishes as with no cable. What the
     * other side started never arrives, as when a real cable is pulled. */
    if (l->out_active && (ctx->io[0x02] & 0x81) == 0x81) {
        ctx->serial_transfer.active = 1;
        ctx->serial_transfer.fast_clock = 0;
        ctx->serial_transfer.cycles_remaining = 4096u;
    }
    if (l->net) gb_net_close(l->net);
    free(l);
}

bool gbrt_link_wait(GBContext* ctx, int timeout_ms)
{
    GBLink* l = ctx->link;
    if (!l) return true;
    pump(l);
    update(l);
    if (!l->stall) return true;
    const uint64_t deadline = clock_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    for (;;) {
        const uint64_t t = clock_us();
        const int remaining = t >= deadline ? 0 : (int)((deadline - t + 999) / 1000);
        GBLinkEvent ev;
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

void gbrt_link_idle(GBContext* ctx, int timeout_ms)
{
    GBLink* l = ctx->link;
    if (!l) return;
    GBLinkEvent ev;
    if (l->io.poll(l->io.ctx, timeout_ms, &ev) > 0) {
        handle(l, &ev);
        pump(l);
    }
    update(l);
}

void gbrt_link_status(const GBContext* ctx, GBLinkStatus* out)
{
    memset(out, 0, sizeof *out);
    out->self = -1;
    const GBLink* l = ctx ? ctx->link : NULL;
    if (!l) return;
    out->attached = true;
    out->self = l->self;
    out->players = players(l);
    out->stalled = l->stall != 0;
    out->transfers = l->transfers;
    out->time = link_now(l);
}

/* ------------------------------------------------------------------ */
/* Command line and network                                            */
/* ------------------------------------------------------------------ */

static struct {
    bool host, local;
    const char* join;
    int port;
    int player;
} g_args = { false, false, NULL, GB_NET_DEFAULT_PORT, 0 };

bool gbrt_link_parse_arg(int argc, char** argv, int* i)
{
    const char* a = argv[*i];
    const bool has_value = *i + 1 < argc;
    if (!strcmp(a, "--link-host")) g_args.host = true;
    else if (!strcmp(a, "--link-local")) g_args.local = true;
    else if (!strcmp(a, "--link-join") && has_value) g_args.join = argv[++*i];
    else if (!strcmp(a, "--link-port") && has_value) {
        int port = atoi(argv[++*i]);
        g_args.port = port < 1024 ? 1024 : port > 65535 ? 65535 : port;
    } else if (!strcmp(a, "--player") && has_value) {
        int player = atoi(argv[++*i]);
        g_args.player = player < 1 ? 1 : player > 4 ? 4 : player;
    } else {
        return false;
    }
    return true;
}

int gbrt_link_player(void)
{
    return g_args.player;
}

uint16_t gbrt_link_port(void)
{
    return (uint16_t)g_args.port;
}

bool gbrt_link_start_from_args(GBContext* ctx)
{
    char err[200];
    if (g_args.host) {
        if (!gbrt_link_host(ctx, (uint16_t)g_args.port, g_args.local, err, sizeof err)) {
            fprintf(stderr, "[LINK] cannot host: %s\n", err);
            return false;
        }
        return true;
    }
    if (g_args.join) return gbrt_link_join(ctx, g_args.join);
    return true;
}

bool gbrt_link_host(GBContext* ctx, uint16_t port, bool local_only, char* err, size_t cap)
{
    char game[64];
    gbrt_link_game_id(ctx, game, sizeof game);
    GBNet* net = gb_net_host(port, local_only, game, err, cap);
    if (!net) return false;
    GBLink* l = gbrt_link_attach(ctx, gb_net_io(net), gb_net_initial_player(net), local_only);
    if (!l) {
        gb_net_close(net);
        snprintf(err, cap, "out of memory");
        return false;
    }
    l->net = net;
    return true;
}

bool gbrt_link_join(GBContext* ctx, const char* address)
{
    char game[64];
    gbrt_link_game_id(ctx, game, sizeof game);
    GBNet* net = gb_net_join(address, game);
    if (!net) return false;
    const bool local = !strncmp(address, "127.", 4) || !strncmp(address, "localhost", 9);
    GBLink* l = gbrt_link_attach(ctx, gb_net_io(net), gb_net_initial_player(net), local);
    if (!l) {
        gb_net_close(net);
        return false;
    }
    l->net = net;
    return true;
}

void gbrt_link_stop(GBContext* ctx)
{
    gbrt_link_detach(ctx);
}

const char* gbrt_link_net_status(const GBContext* ctx)
{
    return ctx && ctx->link && ctx->link->net ? gb_net_status(ctx->link->net) : "";
}

bool gbrt_link_net_is_host(const GBContext* ctx)
{
    return ctx && ctx->link && ctx->link->net && gb_net_is_host(ctx->link->net);
}
