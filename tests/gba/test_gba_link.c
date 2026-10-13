/*
 * Link cable in multi-player mode, with machines in one process joined by
 * an in-memory transport. The CPU spins on a branch-to-self while the test
 * drives the serial registers the way games do: every machine selects
 * multi-player mode with the serial interrupt enabled, puts its halfword in
 * SIOMLT_SEND, and the parent sets SIOCNT bit 7.
 *
 * Checks: the registers each machine sees (SI, SD, ID, busy, SIOMULTI0-3,
 * the serial interrupt), a transfer taking its time on the wire, the
 * lockstep bound (a child never more than the lookahead past the parent),
 * a hundred back-to-back transfers with changing data, three players, and
 * a player leaving in the middle of a transfer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"
#include "gba_link.h"

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

#define PLAYERS 3
#define INBOX 4096

typedef struct Node {
    GbaMachine *m;
    GbaLink *link;
    int id;
    GbaLinkEvent inbox[INBOX];
    unsigned head, count;
    bool connected;
} Node;

static Node nodes[PLAYERS];
static int node_count;

static void deliver(Node *to, const GbaLinkEvent *ev)
{
    if (to->count == INBOX) {
        printf("FAIL inbox overflow\n");
        failures++;
        exit(1);
    }
    to->inbox[(to->head + to->count) % INBOX] = *ev;
    to->count++;
}

static void mem_send(void *ctx, const uint8_t *data, size_t len)
{
    Node *from = ctx;
    GbaLinkEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GBA_LINK_EVENT_MESSAGE;
    ev.from = from->id;
    ev.len = (uint8_t)len;
    memcpy(ev.data, data, len);
    for (int i = 0; i < node_count; i++) {
        if (&nodes[i] != from && nodes[i].connected) deliver(&nodes[i], &ev);
    }
}

static int mem_poll(void *ctx, int timeout_ms, GbaLinkEvent *ev)
{
    (void)timeout_ms;
    Node *n = ctx;
    if (!n->count) return 0;
    *ev = n->inbox[n->head];
    n->head = (n->head + 1) % INBOX;
    n->count--;
    return 1;
}

static uint8_t present_mask(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].connected) mask |= (uint8_t)(1u << nodes[i].id);
    }
    return mask;
}

static void roster_changed(void)
{
    for (int i = 0; i < node_count; i++) {
        if (!nodes[i].connected) continue;
        GbaLinkEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = GBA_LINK_EVENT_ROSTER;
        ev.self = nodes[i].id;
        ev.present = present_mask();
        deliver(&nodes[i], &ev);
    }
}

static const uint8_t k_rom[16] = {
    0xFE, 0xFF, 0xFF, 0xEA, /* b . */
};

static void wait_linked(void);

static void setup(int count)
{
    for (int i = 0; i < node_count; i++) {
        gba_link_detach(nodes[i].m);
        gba_destroy(nodes[i].m);
    }
    memset(nodes, 0, sizeof nodes);
    node_count = count;
    for (int i = 0; i < count; i++) {
        Node *n = &nodes[i];
        n->id = i;
        n->m = gba_create();
        uint8_t rom[0x200];
        memset(rom, 0, sizeof rom);
        memcpy(rom, k_rom, sizeof k_rom);
        gba_load_rom(n->m, rom, sizeof rom);
        gba_reset(n->m, true);
        /* Children start later than the parent, as a second window would. */
        gba_run_cycles(n->m, 50000u * (uint64_t)(count - i));
        GbaLinkIo io = { n, mem_send, mem_poll };
        n->link = gba_link_attach(n->m, io, i == 0 ? 0 : -1);
        n->connected = true;
    }
    roster_changed();
    wait_linked();
}

static bool stalled(const Node *n)
{
    return (n->m->attention & GBA_ATTENTION_STALL) != 0;
}

static int64_t link_time(const Node *n)
{
    GbaLinkStatus s;
    gba_link_status(n->link, &s);
    return s.time;
}

static int64_t max_lead; /* largest child - parent link time seen */

/* One round: every machine runs up to `cycles`, or until it stalls. */
static void round_robin(uint64_t cycles)
{
    for (int i = 0; i < node_count; i++) {
        Node *n = &nodes[i];
        if (!n->connected) continue;
        if (stalled(n)) gba_link_wait(n->link, 0);
        /* Children get longer slices, so they press against the bound. */
        if (!stalled(n)) gba_run_cycles(n->m, i == 0 ? cycles : cycles * 8);
        else gba_link_wait(n->link, 0);
    }
    for (int i = 1; i < node_count; i++) {
        if (!nodes[i].connected || !nodes[0].connected) continue;
        GbaLinkStatus s;
        gba_link_status(nodes[i].link, &s);
        if (s.self <= 0 || !gba_link_follows(nodes[i].link)) continue;
        int64_t lead = link_time(&nodes[i]) - link_time(&nodes[0]);
        if (lead > max_lead) max_lead = lead;
    }
}

/* Until every machine knows the others are there and the children have
 * the parent's clock. */
static void wait_linked(void)
{
    for (int guard = 0; guard < 100000; guard++) {
        bool all = true;
        for (int i = 0; i < node_count; i++) {
            GbaLinkStatus s;
            gba_link_status(nodes[i].link, &s);
            if (s.players != node_count || (i > 0 && !gba_link_follows(nodes[i].link))) all = false;
        }
        if (all) return;
        round_robin(256);
    }
    printf("FAIL the machines never linked\n");
    failures++;
}

static void io_write(Node *n, uint32_t off, uint16_t v) { gba_io_write16(n->m, off, v, 0xFFFF); }
static uint16_t io_read(Node *n, uint32_t off) { return gba_io_read16(n->m, off); }

static void multi_mode(Node *n)
{
    io_write(n, 0x134, 0x0000);          /* RCNT: serial */
    io_write(n, 0x128, 0x6003);          /* multi-player, 115200 bps, IRQ */
    io_write(n, 0x200, GBA_IRQ_SERIAL);  /* IE (IME stays off: IF just records it) */
}

/* Run until every machine's clock passed the parent's `until` link time. */
static void run_until(int64_t until)
{
    for (int guard = 0; guard < 200000; guard++) {
        bool done = true;
        for (int i = 0; i < node_count; i++) {
            if (nodes[i].connected && link_time(&nodes[i]) < until) done = false;
        }
        if (done) return;
        round_robin(256);
    }
    printf("FAIL run_until: machines stopped making progress\n");
    failures++;
}

static void test_two_players(void)
{
    setup(2);
    Node *p = &nodes[0], *c = &nodes[1];
    run_until(link_time(p) + 20000);
    multi_mode(p);
    multi_mode(c);
    io_write(p, 0x12A, 0x1111);
    io_write(c, 0x12A, 0x2222);
    run_until(link_time(p) + 10000);

    uint16_t pc = io_read(p, 0x128), cc = io_read(c, 0x128);
    CHECK(!(pc & 4) && (cc & 4), "SI: parent %04X child %04X", pc, cc);
    CHECK((pc & 8) && (cc & 8), "SD (all ready): parent %04X child %04X", pc, cc);

    int64_t start = link_time(p);
    uint32_t duration = gba_link_transfer_cycles(3, 2);
    io_write(p, 0x128, 0x6083);
    CHECK(io_read(p, 0x128) & 0x80, "parent busy after start");
    CHECK(io_read(p, 0x120) == 0xFFFF, "SIOMULTI0 reset at start: %04X", io_read(p, 0x120));

    /* The child shows busy once it reaches the start. */
    bool child_busy = false;
    int64_t child_done = -1, parent_done = -1;
    for (int guard = 0; guard < 100000 && (child_done < 0 || parent_done < 0); guard++) {
        round_robin(16);
        if (io_read(c, 0x128) & 0x80) child_busy = true;
        if (child_done < 0 && (c->m->if_ & GBA_IRQ_SERIAL)) child_done = link_time(c);
        if (parent_done < 0 && (p->m->if_ & GBA_IRQ_SERIAL)) parent_done = link_time(p);
    }
    CHECK(child_busy, "child never showed busy");
    CHECK(parent_done >= start + duration && parent_done < start + duration + 64,
          "parent completed at +%lld, want +%u", (long long)(parent_done - start), duration);
    /* The child is sampled every 128 cycles here. */
    CHECK(child_done >= start + duration && child_done < start + duration + 192,
          "child completed at +%lld, want +%u", (long long)(child_done - start), duration);
    for (int i = 0; i < 2; i++) {
        Node *n = &nodes[i];
        CHECK(io_read(n, 0x120) == 0x1111 && io_read(n, 0x122) == 0x2222 && io_read(n, 0x124) == 0xFFFF &&
                  io_read(n, 0x126) == 0xFFFF,
              "player %d SIOMULTI %04X %04X %04X %04X", i + 1, io_read(n, 0x120), io_read(n, 0x122),
              io_read(n, 0x124), io_read(n, 0x126));
        uint16_t cnt = io_read(n, 0x128);
        CHECK(!(cnt & 0x80), "player %d still busy", i + 1);
        CHECK(((cnt >> 4) & 3) == i, "player %d ID bits %u", i + 1, (cnt >> 4) & 3);
    }
}

/* Games refill SIOMLT_SEND in the serial interrupt and start the next
 * transfer from the parent's; each machine must see every exchange. */
static void test_back_to_back(void)
{
    setup(2);
    Node *p = &nodes[0], *c = &nodes[1];
    multi_mode(p);
    multi_mode(c);
    run_until(link_time(p) + 5000);
    int ok = 0;
    for (int k = 0; k < 100; k++) {
        io_write(p, 0x12A, (uint16_t)(0x1000 + k));
        io_write(c, 0x12A, (uint16_t)(0x2000 + k));
        p->m->if_ = c->m->if_ = 0;
        /* Let the child reach the point where it wrote its value. */
        run_until(link_time(c));
        io_write(p, 0x128, 0x6083);
        for (int guard = 0; guard < 100000 && !((p->m->if_ & c->m->if_) & GBA_IRQ_SERIAL); guard++) round_robin(64);
        bool good = io_read(p, 0x120) == 0x1000 + k && io_read(p, 0x122) == 0x2000 + k &&
                    io_read(c, 0x120) == 0x1000 + k && io_read(c, 0x122) == 0x2000 + k;
        if (!good && ok == k) {
            printf("FAIL transfer %d: parent %04X %04X child %04X %04X\n", k, io_read(p, 0x120), io_read(p, 0x122),
                   io_read(c, 0x120), io_read(c, 0x122));
            failures++;
        }
        ok += good;
        /* Some time in between, as a game's frame would have. */
        run_until(link_time(p) + 3000 + 997 * (k % 7));
    }
    CHECK(ok == 100, "%d of 100 transfers exchanged the right data", ok);
    GbaLinkStatus s;
    gba_link_status(c->link, &s);
    CHECK(s.transfers == 100, "child counted %llu transfers", (unsigned long long)s.transfers);
}

static void test_three_players(void)
{
    setup(3);
    for (int i = 0; i < 3; i++) {
        multi_mode(&nodes[i]);
        io_write(&nodes[i], 0x12A, (uint16_t)(0xA000 + i));
    }
    run_until(link_time(&nodes[0]) + 20000);
    io_write(&nodes[0], 0x128, 0x6083);
    for (int guard = 0; guard < 100000; guard++) {
        bool all = true;
        for (int i = 0; i < 3; i++) all = all && (nodes[i].m->if_ & GBA_IRQ_SERIAL);
        if (all) break;
        round_robin(64);
    }
    for (int i = 0; i < 3; i++) {
        Node *n = &nodes[i];
        CHECK(io_read(n, 0x120) == 0xA000 && io_read(n, 0x122) == 0xA001 && io_read(n, 0x124) == 0xA002 &&
                  io_read(n, 0x126) == 0xFFFF,
              "3 players, player %d: %04X %04X %04X %04X", i + 1, io_read(n, 0x120), io_read(n, 0x122),
              io_read(n, 0x124), io_read(n, 0x126));
        CHECK(((io_read(n, 0x128) >> 4) & 3) == i, "3 players, player %d ID", i + 1);
    }
}

/* The child's window closes during a transfer: the parent finishes it
 * without that word instead of waiting forever. */
static void test_player_leaves(void)
{
    setup(2);
    Node *p = &nodes[0], *c = &nodes[1];
    multi_mode(p);
    multi_mode(c);
    io_write(p, 0x12A, 0x1234);
    run_until(link_time(p) + 20000);
    c->connected = false; /* gone before it could answer */
    c->count = 0;
    io_write(p, 0x128, 0x6083);
    for (int guard = 0; guard < 1000 && !(p->m->if_ & GBA_IRQ_SERIAL); guard++) {
        if (guard == 10) roster_changed(); /* the transport notices */
        round_robin(1024);
    }
    CHECK(p->m->if_ & GBA_IRQ_SERIAL, "parent never finished the transfer");
    CHECK(io_read(p, 0x120) == 0x1234 && io_read(p, 0x122) == 0xFFFF, "after leaving: %04X %04X",
          io_read(p, 0x120), io_read(p, 0x122));
    CHECK(!(io_read(p, 0x128) & 8), "SD still reports all GBAs ready");
    /* Alone, the parent runs freely. */
    uint64_t before = p->m->cpu.cycles;
    gba_run_cycles(p->m, 10 * GBA_CYCLES_PER_FRAME);
    CHECK(!stalled(p) && p->m->cpu.cycles >= before + 10 * GBA_CYCLES_PER_FRAME, "parent alone still stalls");
}

/* Without a cable the serial registers are plain storage, as before. */
static void test_unlinked(void)
{
    GbaMachine *m = gba_create();
    gba_io_write16(m, 0x134, 0, 0xFFFF);
    gba_io_write16(m, 0x128, 0x6083, 0xFFFF);
    CHECK(gba_io_read16(m, 0x128) == 0x6083, "unlinked SIOCNT reads %04X", gba_io_read16(m, 0x128));
    gba_io_write16(m, 0x12A, 0x1234, 0xFFFF);
    CHECK(gba_io_read16(m, 0x12A) == 0x0034, "unlinked SIODATA8 reads %04X", gba_io_read16(m, 0x12A));
    gba_run_cycles(m, 100000);
    CHECK(m->sio_at == UINT64_MAX && !(m->if_ & GBA_IRQ_SERIAL), "unlinked machine scheduled serial events");
    gba_destroy(m);
}

int main(void)
{
    test_unlinked();
    test_two_players();
    test_back_to_back();
    test_three_players();
    test_player_leaves();
    CHECK(max_lead <= 4096 + 64, "a child ran %lld cycles past the parent", (long long)max_lead);
    printf("largest child lead over the parent: %lld cycles\n", (long long)max_lead);
    for (int i = 0; i < node_count; i++) {
        gba_link_detach(nodes[i].m);
        gba_destroy(nodes[i].m);
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("link cable: all checks passed\n");
    return 0;
}
