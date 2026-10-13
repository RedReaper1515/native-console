/*
 * Game Boy link cable (gbrt_link.c): two machines in one process, joined by
 * an in-memory transport, run tests/fixtures/make_link_rom.py's ROM with the
 * interpreter. One side drives the clock and the other waits on it; each
 * checks every byte it receives. Covers both directions (host or joiner
 * driving the clock), the lockstep bound, a side alone (reads $FF), and
 * the plain serial port without a cable.
 *
 *   test_gb_link LINKTEST.gb
 */
#include "gbrt.h"
#include "gbrt_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

#define INBOX 8192

typedef struct Node {
    GBContext* ctx;
    int id;
    GBLinkEvent inbox[INBOX];
    unsigned head, count;
    bool connected;
} Node;

static Node nodes[2];
static uint8_t* rom;
static size_t rom_size;

static void deliver(Node* to, const GBLinkEvent* ev)
{
    if (to->count == INBOX) {
        printf("FAIL inbox overflow\n");
        exit(1);
    }
    to->inbox[(to->head + to->count) % INBOX] = *ev;
    to->count++;
}

static void mem_send(void* user, const uint8_t* data, size_t len)
{
    Node* from = (Node*)user;
    Node* to = &nodes[1 - from->id];
    if (!to->connected) return;
    GBLinkEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GB_LINK_EVENT_MESSAGE;
    ev.from = from->id;
    ev.len = (uint8_t)len;
    memcpy(ev.data, data, len);
    deliver(to, &ev);
}

static int mem_poll(void* user, int timeout_ms, GBLinkEvent* ev)
{
    (void)timeout_ms;
    Node* n = (Node*)user;
    if (!n->count) return 0;
    *ev = n->inbox[n->head];
    n->head = (n->head + 1) % INBOX;
    n->count--;
    return 1;
}

static void roster(int count)
{
    for (int i = 0; i < 2; i++) {
        if (!nodes[i].connected) continue;
        GBLinkEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = GB_LINK_EVENT_ROSTER;
        ev.self = i;
        ev.present = (uint8_t)(count == 2 ? 3 : 1u << i);
        deliver(&nodes[i], &ev);
    }
}

static GBContext* make_machine(void)
{
    GBConfig config;
    memset(&config, 0, sizeof config);
    config.enable_serial = true;
    GBContext* ctx = gb_context_create(&config);
    if (!ctx || !gb_context_load_rom(ctx, rom, rom_size)) {
        printf("FAIL cannot create a machine\n");
        exit(2);
    }
    gb_context_reset(ctx, true);
    return ctx;
}

static void setup(int clock_side, bool link)
{
    for (int i = 0; i < 2; i++) {
        if (nodes[i].ctx) gb_context_destroy(nodes[i].ctx);
    }
    memset(nodes, 0, sizeof nodes);
    for (int i = 0; i < 2; i++) {
        Node* n = &nodes[i];
        n->id = i;
        n->ctx = make_machine();
        n->ctx->hram[0x06] = (uint8_t)(i == clock_side); /* FF86: drive the clock */
        /* The joiner starts later, as a second window would. */
        if (i == 1) gb_run_cycles(n->ctx, 40000);
        if (link) {
            GBLinkIo io = { n, mem_send, mem_poll };
            gbrt_link_attach(n->ctx, io, i == 0 ? 0 : -1, true);
            n->connected = true;
        }
    }
    if (link) roster(2);
}

static int64_t max_lead;

/* Each machine runs a slice, or waits if the link holds it back. */
static void step(uint32_t cycles)
{
    for (int i = 0; i < 2; i++) {
        GBContext* ctx = nodes[i].ctx;
        if (ctx->frame_done) gb_reset_frame(ctx);
        if (gbrt_link_stalled(ctx)) gbrt_link_wait(ctx, 0);
        if (!gbrt_link_stalled(ctx)) gb_run_cycles(ctx, cycles * (i == 1 ? 3u : 1u));
    }
    GBLinkStatus a, b;
    gbrt_link_status(nodes[0].ctx, &a);
    gbrt_link_status(nodes[1].ctx, &b);
    if (a.players == 2 && b.players == 2 && a.transfers > 0 && b.transfers > 0) {
        int64_t lead = b.time - a.time;
        if (lead < 0) lead = -lead;
        if (lead > max_lead) max_lead = lead;
    }
}

static void run_exchange(int clock_side, const char* name)
{
    setup(clock_side, true);
    const uint8_t* clock_hram = nodes[clock_side].ctx->hram;
    const uint8_t* wait_hram = nodes[1 - clock_side].ctx->hram;
    for (int guard = 0; guard < 400000 && (clock_hram[0x05] < 200 || wait_hram[0x00] < 200); guard++) step(456);
    CHECK(clock_hram[0x02] == 1 && wait_hram[0x02] == 2, "%s: roles %u/%u", name, clock_hram[2], wait_hram[2]);
    CHECK(clock_hram[0x05] >= 200, "%s: only %u good replies", name, clock_hram[5]);
    CHECK(clock_hram[0x03] == 0, "%s: clock side got %u wrong bytes (last $%02X)", name, clock_hram[3], clock_hram[4]);
    CHECK(wait_hram[0x03] == 0, "%s: waiting side got %u wrong bytes (last $%02X)", name, wait_hram[3], wait_hram[4]);
    GBLinkStatus s, j;
    gbrt_link_status(nodes[0].ctx, &s);
    gbrt_link_status(nodes[1].ctx, &j);
    printf("%s: %u exchanges, host counted %llu bytes\n", name, wait_hram[0], (unsigned long long)s.transfers);
    if (failures) {
        printf("  host: players %d stalled %d time %lld | joiner: self %d players %d stalled %d time %lld\n", s.players,
               s.stalled, (long long)s.time, j.self, j.players, j.stalled, (long long)j.time);
        printf("  clock side hram %02X %02X %02X %02X %02X %02X SC %02X | waiting side %02X %02X %02X SC %02X\n",
               clock_hram[0], clock_hram[1], clock_hram[2], clock_hram[3], clock_hram[4], clock_hram[5],
               nodes[clock_side].ctx->io[2], wait_hram[0], wait_hram[1], wait_hram[2], nodes[1 - clock_side].ctx->io[2]);
    }
}

static void test_alone(void)
{
    /* A cable with nobody on the other end: the clock side reads $FF. */
    setup(0, true);
    nodes[1].connected = false;
    nodes[0].count = 0;
    roster(1);
    for (int i = 0; i < 2000; i++) {
        GBContext* ctx = nodes[0].ctx;
        if (ctx->frame_done) gb_reset_frame(ctx);
        gb_run_cycles(ctx, 456);
    }
    const uint8_t* h = nodes[0].ctx->hram;
    CHECK(h[0x02] == 1 && h[0x01] == 0xFF && h[0x05] == 0, "alone: role %u last $%02X good %u", h[2], h[1], h[5]);
    CHECK(!gbrt_link_stalled(nodes[0].ctx), "alone: still stalled");
}

static void test_no_cable(void)
{
    /* Without a link the serial port works as before: $FF comes back. */
    setup(0, false);
    for (int i = 0; i < 2000; i++) {
        GBContext* ctx = nodes[0].ctx;
        if (ctx->frame_done) gb_reset_frame(ctx);
        gb_run_cycles(ctx, 456);
    }
    const uint8_t* h = nodes[0].ctx->hram;
    CHECK(h[0x02] == 1 && h[0x01] == 0xFF && h[0x05] == 0 && h[0x03] == 0, "no cable: role %u last $%02X good %u",
          h[2], h[1], h[5]);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_gb_link LINKTEST.gb\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = (uint8_t*)malloc(rom_size);
    if (!rom || fread(rom, 1, rom_size, f) != rom_size) return 2;
    fclose(f);

    run_exchange(0, "host drives the clock");
    run_exchange(1, "joiner drives the clock");
    CHECK(max_lead <= 1536 + 64, "one machine ran %lld cycles past the other", (long long)max_lead);
    printf("largest gap between the machines: %lld cycles\n", (long long)max_lead);
    test_alone();
    test_no_cable();
    for (int i = 0; i < 2; i++) gb_context_destroy(nodes[i].ctx);
    free(rom);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("gb link cable: all checks passed\n");
    return 0;
}
