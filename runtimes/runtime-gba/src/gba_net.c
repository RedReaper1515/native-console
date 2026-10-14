/*
 * TCP transport for the link cable; see gba_net.h.
 */
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
typedef SOCKET net_socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
static bool would_block(void)
{
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int net_socket;
#define BAD_SOCKET (-1)
#define close_socket close
static bool would_block(void)
{
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}
#endif

#include "gba_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define PROTOCOL_VERSION 1
#define MAX_CLIENTS (GBA_LINK_MAX_PLAYERS - 1)
#define EVENT_QUEUE 256
#define RETRY_MS 1000
#define CONTROL 0xFFu

enum { CLIENT_CONNECTING, CLIENT_HANDSHAKE, CLIENT_JOINED, CLIENT_REFUSED };

typedef struct Conn {
    net_socket s;
    bool used;
    bool joined;
    int player; /* 1-3 once joined */
    uint8_t buf[1024];
    size_t len;
} Conn;

struct GbaNet {
    bool host;
    char game[64];
    uint16_t port;
    /* Host. */
    net_socket listener;
    Conn clients[MAX_CLIENTS];
    /* Joiner. */
    char address[256];
    Conn server;
    int state;
    bool connect_pending;
    uint64_t retry_at;
    unsigned attempts;
    int self;
    uint8_t present;

    GbaLinkEvent events[EVENT_QUEUE];
    uint64_t due[EVENT_QUEUE]; /* when each may be delivered (GBA_NET_DELAY_MS) */
    unsigned ev_head, ev_count;
    uint64_t delay_us;
    char status[256];
};

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

static uint64_t clock_ms(void)
{
    return clock_us() / 1000u;
}

/* GBA_NET_DELAY_MS=N holds every received message back N ms (fractions
 * allowed): a test aid that shows how the lockstep copes with a network's
 * latency without a second computer. */
static uint64_t test_delay_us(void)
{
    const char *text = getenv("GBA_NET_DELAY_MS");
    double ms = text ? atof(text) : 0.0;
    return ms > 0.0 ? (uint64_t)(ms * 1000.0) : 0u;
}

static bool net_init(void)
{
#ifdef _WIN32
    static bool ready;
    if (!ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        ready = true;
    }
#endif
    return true;
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void set_nonblocking(net_socket s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
}

static void push_event(GbaNet *n, const GbaLinkEvent *ev)
{
    if (n->ev_count == EVENT_QUEUE) return; /* cannot happen at link message rates */
    unsigned k = (n->ev_head + n->ev_count) % EVENT_QUEUE;
    n->events[k] = *ev;
    n->due[k] = n->delay_us ? clock_us() + n->delay_us : 0;
    n->ev_count++;
}

static bool pop_event(GbaNet *n, GbaLinkEvent *ev)
{
    if (!n->ev_count || (n->due[n->ev_head] && n->due[n->ev_head] > clock_us())) return false;
    *ev = n->events[n->ev_head];
    n->ev_head = (n->ev_head + 1) % EVENT_QUEUE;
    n->ev_count--;
    return true;
}

static void push_roster(GbaNet *n, int self, uint8_t present)
{
    GbaLinkEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GBA_LINK_EVENT_ROSTER;
    ev.self = self;
    ev.present = present;
    push_event(n, &ev);
}

/* Whole frame or nothing; a peer that stops reading for a second is gone. */
static bool send_frame(net_socket s, uint8_t origin, const uint8_t *data, size_t len)
{
    uint8_t frame[2 + 255];
    if (len > 254) return false;
    frame[0] = (uint8_t)(len + 1);
    frame[1] = origin;
    memcpy(frame + 2, data, len);
    size_t total = len + 2, done = 0;
    uint64_t give_up = clock_ms() + 1000;
    while (done < total) {
        int r = (int)send(s, (const char *)frame + done, (int)(total - done), MSG_NOSIGNAL);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r < 0 && would_block() && clock_ms() < give_up) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            struct timeval tv = { 0, 10000 };
            select((int)s + 1, NULL, &w, NULL, &tv);
            continue;
        }
        return false;
    }
    return true;
}

static void close_conn(Conn *c)
{
    if (c->used && c->s != BAD_SOCKET) close_socket(c->s);
    c->s = BAD_SOCKET;
    c->used = c->joined = false;
    c->len = 0;
    c->player = 0;
}

/* ------------------------------------------------------------------ */
/* Host                                                                */
/* ------------------------------------------------------------------ */

static uint8_t host_present(const GbaNet *n)
{
    uint8_t present = 1u;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (n->clients[i].joined) present |= (uint8_t)(1u << n->clients[i].player);
    }
    return present;
}

static void host_update_status(GbaNet *n)
{
    int joined = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) joined += n->clients[i].joined;
    if (joined) snprintf(n->status, sizeof n->status, "Hosting on port %u: %d players linked", n->port, joined + 1);
    else snprintf(n->status, sizeof n->status, "Hosting on port %u, waiting for players", n->port);
}

static void host_roster_changed(GbaNet *n)
{
    uint8_t present = host_present(n);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Conn *c = &n->clients[i];
        if (!c->joined) continue;
        uint8_t msg[3] = { 'R', (uint8_t)c->player, present };
        send_frame(c->s, CONTROL, msg, sizeof msg);
    }
    push_roster(n, 0, present);
    host_update_status(n);
}

static void host_drop(GbaNet *n, Conn *c)
{
    bool was_joined = c->joined;
    close_conn(c);
    if (was_joined) host_roster_changed(n);
}

static void host_refuse(GbaNet *n, Conn *c, const char *reason)
{
    send_frame(c->s, CONTROL, (const uint8_t *)reason, strlen(reason));
    (void)n;
    close_conn(c);
}

static void host_frame(GbaNet *n, Conn *c, const uint8_t *frame, size_t len)
{
    if (len < 1) return;
    uint8_t origin = frame[0];
    const uint8_t *payload = frame + 1;
    size_t plen = len - 1;
    if (origin == CONTROL) {
        if (plen < 2 || payload[0] != 'H' || c->joined) return;
        char game[64];
        size_t glen = plen - 2 < sizeof game - 1 ? plen - 2 : sizeof game - 1;
        memcpy(game, payload + 2, glen);
        game[glen] = '\0';
        char reason[200];
        if (payload[1] != PROTOCOL_VERSION) {
            snprintf(reason, sizeof reason, "N%s", "the host runs a different Native Console version");
            host_refuse(n, c, reason);
            return;
        }
        if (strcmp(game, n->game) != 0) {
            snprintf(reason, sizeof reason, "Nthe host is playing a different game (%s)", n->game);
            host_refuse(n, c, reason);
            return;
        }
        bool taken[GBA_LINK_MAX_PLAYERS] = { true, false, false, false };
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (n->clients[i].joined) taken[n->clients[i].player] = true;
        }
        int player = 1;
        while (player < GBA_LINK_MAX_PLAYERS && taken[player]) player++;
        if (player == GBA_LINK_MAX_PLAYERS) {
            host_refuse(n, c, "Nthe game already has four players");
            return;
        }
        c->joined = true;
        c->player = player;
        host_roster_changed(n);
        return;
    }
    if (!c->joined || plen == 0 || plen > GBA_LINK_MAX_MESSAGE) return;
    /* Pass it on to the other joiners, then deliver it here. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Conn *o = &n->clients[i];
        if (o != c && o->joined && !send_frame(o->s, (uint8_t)c->player, payload, plen)) host_drop(n, o);
    }
    GbaLinkEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GBA_LINK_EVENT_MESSAGE;
    ev.from = c->player;
    ev.len = (uint8_t)plen;
    memcpy(ev.data, payload, plen);
    push_event(n, &ev);
}

/* Split received bytes into frames. False when the peer closed. */
static bool read_frames(GbaNet *n, Conn *c, void (*on_frame)(GbaNet *, Conn *, const uint8_t *, size_t))
{
    for (;;) {
        int r = (int)recv(c->s, (char *)c->buf + c->len, (int)(sizeof c->buf - c->len), 0);
        if (r == 0) return false;
        if (r < 0) return would_block();
        c->len += (size_t)r;
        size_t pos = 0;
        while (c->len - pos >= 1 && c->len - pos >= 1u + c->buf[pos]) {
            size_t flen = c->buf[pos];
            if (flen) on_frame(n, c, c->buf + pos + 1, flen);
            if (!c->used) return true; /* refused or dropped while handling it */
            pos += 1 + flen;
        }
        memmove(c->buf, c->buf + pos, c->len - pos);
        c->len -= pos;
    }
}

static void host_poll_sockets(GbaNet *n, int timeout_ms)
{
    fd_set r;
    FD_ZERO(&r);
    net_socket top = n->listener;
    FD_SET(n->listener, &r);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!n->clients[i].used) continue;
        FD_SET(n->clients[i].s, &r);
        if (n->clients[i].s > top) top = n->clients[i].s;
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select((int)top + 1, &r, NULL, NULL, &tv) <= 0) return;

    if (FD_ISSET(n->listener, &r)) {
        net_socket s = accept(n->listener, NULL, NULL);
        if (s != BAD_SOCKET) {
            Conn *slot = NULL;
            for (int i = 0; i < MAX_CLIENTS && !slot; i++) {
                if (!n->clients[i].used) slot = &n->clients[i];
            }
            if (!slot) {
                close_socket(s);
            } else {
                set_nonblocking(s);
                memset(slot, 0, sizeof *slot);
                slot->s = s;
                slot->used = true;
            }
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Conn *c = &n->clients[i];
        if (c->used && FD_ISSET(c->s, &r) && !read_frames(n, c, host_frame)) host_drop(n, c);
    }
}

GbaNet *gba_net_host(uint16_t port, bool loopback_only, const char *game_id, char *err, size_t cap)
{
    if (!net_init()) {
        snprintf(err, cap, "networking is unavailable");
        return NULL;
    }
    net_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCKET) {
        snprintf(err, cap, "cannot create a socket");
        return NULL;
    }
#ifndef _WIN32
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, 4) != 0) {
        close_socket(s);
        snprintf(err, cap, "port %u is in use (is another game hosting?)", port);
        return NULL;
    }
    set_nonblocking(s);
    GbaNet *n = calloc(1, sizeof *n);
    if (!n) {
        close_socket(s);
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    n->host = true;
    n->delay_us = test_delay_us();
    n->port = port;
    n->listener = s;
    for (int i = 0; i < MAX_CLIENTS; i++) n->clients[i].s = BAD_SOCKET;
    snprintf(n->game, sizeof n->game, "%s", game_id);
    n->self = 0;
    n->present = 1;
    host_update_status(n);
    return n;
}

/* ------------------------------------------------------------------ */
/* Joiner                                                              */
/* ------------------------------------------------------------------ */

static void client_lost(GbaNet *n, const char *why)
{
    bool was_joined = n->state == CLIENT_JOINED;
    close_conn(&n->server);
    n->connect_pending = false;
    n->state = CLIENT_CONNECTING;
    n->retry_at = clock_ms() + RETRY_MS;
    n->self = -1;
    n->present = 0;
    if (was_joined) push_roster(n, -1, 0);
    snprintf(n->status, sizeof n->status, "%s; retrying %s", why, n->address);
}

static void client_frame(GbaNet *n, Conn *c, const uint8_t *frame, size_t len)
{
    (void)c;
    if (len < 2) return;
    uint8_t origin = frame[0];
    const uint8_t *payload = frame + 1;
    size_t plen = len - 1;
    if (origin == CONTROL) {
        if (payload[0] == 'R' && plen >= 3) {
            n->self = payload[1];
            n->present = payload[2];
            n->state = CLIENT_JOINED;
            int count = 0;
            for (int i = 0; i < GBA_LINK_MAX_PLAYERS; i++) count += (n->present >> i) & 1;
            snprintf(n->status, sizeof n->status, "Linked to %s as player %d (%d players)", n->address, n->self + 1,
                     count);
            push_roster(n, n->self, n->present);
        } else if (payload[0] == 'N') {
            char reason[200];
            size_t rl = plen - 1 < sizeof reason - 1 ? plen - 1 : sizeof reason - 1;
            memcpy(reason, payload + 1, rl);
            reason[rl] = '\0';
            bool was_joined = n->state == CLIENT_JOINED;
            close_conn(&n->server);
            n->state = CLIENT_REFUSED;
            n->self = -1;
            n->present = 0;
            if (was_joined) push_roster(n, -1, 0);
            snprintf(n->status, sizeof n->status, "Refused: %s", reason);
        }
        return;
    }
    if (n->state != CLIENT_JOINED || plen > GBA_LINK_MAX_MESSAGE || origin >= GBA_LINK_MAX_PLAYERS) return;
    GbaLinkEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = GBA_LINK_EVENT_MESSAGE;
    ev.from = origin;
    ev.len = (uint8_t)plen;
    memcpy(ev.data, payload, plen);
    push_event(n, &ev);
}

static bool parse_address(const char *text, char *host, size_t cap, uint16_t *port)
{
    *port = GBA_NET_DEFAULT_PORT;
    snprintf(host, cap, "%s", text);
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) {
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535) return false;
        *port = (uint16_t)p;
        *colon = '\0';
    }
    return host[0] != '\0';
}

static void client_start_connect(GbaNet *n)
{
    char host[256];
    uint16_t port;
    n->attempts++;
    if (!parse_address(n->address, host, sizeof host, &port)) {
        n->state = CLIENT_REFUSED;
        snprintf(n->status, sizeof n->status, "Bad address \"%s\" (want host or host:port)", n->address);
        return;
    }
    char port_text[8];
    snprintf(port_text, sizeof port_text, "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_text, &hints, &res) != 0 || !res) {
        n->retry_at = clock_ms() + RETRY_MS;
        snprintf(n->status, sizeof n->status, "Cannot find %s; retrying", host);
        return;
    }
    net_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCKET) {
        freeaddrinfo(res);
        n->retry_at = clock_ms() + RETRY_MS;
        return;
    }
    set_nonblocking(s);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    n->server.s = s;
    n->server.used = true;
    n->server.len = 0;
    if (r == 0) {
        n->connect_pending = false;
    } else if (would_block()) {
        n->connect_pending = true;
    } else {
        close_conn(&n->server);
        n->retry_at = clock_ms() + RETRY_MS;
        return;
    }
    snprintf(n->status, sizeof n->status, "Connecting to %s (attempt %u)", n->address, n->attempts);
    if (!n->connect_pending) {
        uint8_t hello[2 + 64] = { 'H', PROTOCOL_VERSION };
        size_t gl = strlen(n->game);
        memcpy(hello + 2, n->game, gl);
        if (!send_frame(s, CONTROL, hello, 2 + gl)) client_lost(n, "Connection failed");
        else n->state = CLIENT_HANDSHAKE;
    }
}

static void client_poll_sockets(GbaNet *n, int timeout_ms)
{
    if (n->state == CLIENT_REFUSED) {
        sleep_ms(timeout_ms);
        return;
    }
    if (n->state == CLIENT_CONNECTING && !n->server.used) {
        uint64_t now = clock_ms();
        if (now < n->retry_at) {
            int wait = (int)(n->retry_at - now);
            sleep_ms(wait < timeout_ms ? wait : timeout_ms);
            if (clock_ms() < n->retry_at) return;
        }
        client_start_connect(n);
        if (!n->server.used) return;
    }
    net_socket s = n->server.s;
    fd_set r, w, e;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);
    if (n->connect_pending) {
        FD_SET(s, &w);
        FD_SET(s, &e);
    } else {
        FD_SET(s, &r);
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int ready = select((int)s + 1, &r, &w, &e, &tv);
    if (ready <= 0) return;
    if (n->connect_pending) {
        int so_error = 0;
        socklen_t sl = sizeof so_error;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_error, &sl);
        if (FD_ISSET(s, &e) || so_error != 0) {
            client_lost(n, "Host not answering");
            return;
        }
        n->connect_pending = false;
        uint8_t hello[2 + 64] = { 'H', PROTOCOL_VERSION };
        size_t gl = strlen(n->game);
        memcpy(hello + 2, n->game, gl);
        if (!send_frame(s, CONTROL, hello, 2 + gl)) client_lost(n, "Connection failed");
        else n->state = CLIENT_HANDSHAKE;
        return;
    }
    if (FD_ISSET(s, &r) && !read_frames(n, &n->server, client_frame) && n->state != CLIENT_REFUSED) {
        client_lost(n, "Lost the host");
    }
}

GbaNet *gba_net_join(const char *address, const char *game_id)
{
    GbaNet *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->host = false;
    n->delay_us = test_delay_us();
    n->listener = BAD_SOCKET;
    n->server.s = BAD_SOCKET;
    for (int i = 0; i < MAX_CLIENTS; i++) n->clients[i].s = BAD_SOCKET;
    snprintf(n->address, sizeof n->address, "%s", address);
    snprintf(n->game, sizeof n->game, "%s", game_id);
    n->state = CLIENT_CONNECTING;
    n->self = -1;
    snprintf(n->status, sizeof n->status, "Connecting to %s", address);
    if (!net_init()) {
        n->state = CLIENT_REFUSED;
        snprintf(n->status, sizeof n->status, "Networking is unavailable");
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* GbaLinkIo                                                           */
/* ------------------------------------------------------------------ */

static void net_send(void *ctx, const uint8_t *data, size_t len)
{
    GbaNet *n = ctx;
    if (n->host) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Conn *c = &n->clients[i];
            if (c->joined && !send_frame(c->s, 0, data, len)) host_drop(n, c);
        }
    } else if (n->state == CLIENT_JOINED) {
        if (!send_frame(n->server.s, (uint8_t)n->self, data, len)) client_lost(n, "Lost the host");
    }
}

static int net_poll(void *ctx, int timeout_ms, GbaLinkEvent *ev)
{
    GbaNet *n = ctx;
    uint64_t deadline = clock_us() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000u;
    for (;;) {
        if (pop_event(n, ev)) return 1;
        uint64_t now = clock_us(), until = deadline;
        bool held = n->ev_count && n->due[n->ev_head] < until; /* a held-back message (GBA_NET_DELAY_MS) */
        if (held) until = n->due[n->ev_head];
        int wait_ms = now >= until ? 0 : (int)((until - now + 999) / 1000);
        if (held && until - now < 2000u) wait_ms = 0; /* timer sleeps are too coarse: spin */
        if (n->host) host_poll_sockets(n, wait_ms);
        else client_poll_sockets(n, wait_ms);
        if (pop_event(n, ev)) return 1;
        if (clock_us() >= deadline) return 0;
    }
}

GbaLinkIo gba_net_io(GbaNet *n)
{
    GbaLinkIo io = { n, net_send, net_poll };
    return io;
}

int gba_net_initial_player(const GbaNet *n)
{
    return n->host ? 0 : -1;
}

bool gba_net_is_host(const GbaNet *n)
{
    return n->host;
}

const char *gba_net_status(const GbaNet *n)
{
    return n->status;
}

void gba_net_close(GbaNet *n)
{
    if (!n) return;
    if (n->host) {
        for (int i = 0; i < MAX_CLIENTS; i++) close_conn(&n->clients[i]);
        if (n->listener != BAD_SOCKET) close_socket(n->listener);
    } else {
        close_conn(&n->server);
    }
    free(n);
}

void gba_net_local_addresses(char *out, size_t cap)
{
    out[0] = '\0';
    if (!net_init()) return;
    char name[256];
    if (gethostname(name, sizeof name) != 0) return;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name, NULL, &hints, &res) != 0) return;
    size_t used = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)a->ai_addr;
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        if ((ip >> 24) == 127) continue;
        char text[32];
        snprintf(text, sizeof text, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u);
        if (strstr(out, text)) continue;
        int w = snprintf(out + used, cap - used, "%s%s", used ? ", " : "", text);
        if (w < 0 || (size_t)w >= cap - used) break;
        used += (size_t)w;
    }
    freeaddrinfo(res);
}
