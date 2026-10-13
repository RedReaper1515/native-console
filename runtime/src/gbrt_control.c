/*
 * The game played by a program; see gbrt_control.h.
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
typedef SOCKET control_socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int control_socket;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

#include "gbrt_control.h"
#include "gbrt_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define IDLE_MS 20
#define MAX_WATCHED (1u << 20)      /* bytes an F may carry */
#define MAX_MESSAGE (1u << 20)      /* bytes a message to the game may hold */

typedef struct {
    uint16_t address;
    uint16_t length;
} Watch;

static unsigned g_port;
static control_socket g_socket = BAD_SOCKET;
static bool g_started;
static bool g_lost;                 /* the program went away */
static bool g_closing;              /* the program said to close */
static bool (*g_idle)(GBContext* ctx);
static Watch* g_watches;
static size_t g_watch_count;
static size_t g_watched_bytes;
static uint32_t g_frames_left;      /* frames still to run before stopping */
static uint8_t g_held;              /* the buttons, as the program gave them */
static uint8_t* g_out;              /* a message being put together */
static size_t g_out_capacity;
static uint8_t* g_in;
static size_t g_in_capacity;

void gbrt_control_set_port(unsigned port) { g_port = port; }
bool gbrt_control_requested(void) { return g_port != 0; }
void gbrt_control_set_idle(bool (*idle)(GBContext* ctx)) { g_idle = idle; }

static void put32(uint8_t* at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static void put64(uint8_t* at, uint64_t value) {
    put32(at, (uint32_t)value);
    put32(at + 4, (uint32_t)(value >> 32));
}

static uint16_t get16(const uint8_t* at) { return (uint16_t)(at[0] | (at[1] << 8)); }

static uint32_t get32(const uint8_t* at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static void drop(void) {
    if (g_socket != BAD_SOCKET) close_socket(g_socket);
    g_socket = BAD_SOCKET;
}

static void lose(const char* why) {
    if (!g_lost) fprintf(stderr, "[CONTROL] %s; the game closes\n", why);
    g_lost = true;
    drop();
}

static bool send_all(const uint8_t* data, size_t length) {
    while (length > 0) {
        int sent = send(g_socket, (const char*)data, (int)(length > 0x40000000u ? 0x40000000u : length),
                        MSG_NOSIGNAL);
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

/* The message's letter and length, then room for `length` bytes: what
 * send_message() sends is g_out as it then stands. */
static uint8_t* message(char letter, size_t length) {
    if (length + 5 > g_out_capacity) {
        size_t capacity = length + 5 + 4096;
        uint8_t* grown = (uint8_t*)realloc(g_out, capacity);
        if (!grown) return NULL;
        g_out = grown;
        g_out_capacity = capacity;
    }
    g_out[0] = (uint8_t)letter;
    put32(g_out + 1, (uint32_t)length);
    return g_out + 5;
}

static bool send_message(void) {
    if (g_socket == BAD_SOCKET) return false;
    if (!send_all(g_out, 5 + get32(g_out + 1))) {
        lose("the program playing the game is gone");
        return false;
    }
    return true;
}

static bool reply(char letter, const char* text) {
    size_t length = text ? strlen(text) : 0;
    uint8_t* body = message(letter, length);
    if (!body) return false;
    if (length) memcpy(body, text, length);
    return send_message();
}

/* `length` bytes from the program, however long it takes. While nothing
 * comes, the idle callback keeps the window alive and may close the game. */
static bool receive(GBContext* ctx, uint8_t* into, size_t length) {
    while (length > 0) {
        fd_set readable;
        struct timeval wait = {0, IDLE_MS * 1000};
        FD_ZERO(&readable);
        FD_SET(g_socket, &readable);
        int ready = select((int)g_socket + 1, &readable, NULL, NULL, &wait);
        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            lose("the connection to the program playing the game failed");
            return false;
        }
        if (ready == 0) {
            if (g_idle && !g_idle(ctx)) {
                g_closing = true;
                return false;
            }
            continue;
        }
        int got = recv(g_socket, (char*)into, (int)(length > 0x40000000u ? 0x40000000u : length), 0);
        if (got <= 0) {
            lose("the program playing the game is gone");
            return false;
        }
        into += got;
        length -= (size_t)got;
    }
    return true;
}

static uint8_t flags(void) {
    return (uint8_t)((gbrt_run_recording() ? 1 : 0) | (gbrt_run_replaying() ? 2 : 0));
}

/* Frames the game has finished. The loop counts a frame when the next one
 * starts, so one that has just ended is counted here. */
static uint64_t frames_done(const GBContext* ctx) {
    return ctx->completed_frames + (ctx->frame_done ? 1u : 0u);
}

static bool send_frame(GBContext* ctx) {
    uint8_t* body = message('F', 17 + g_watched_bytes);
    if (!body) return false;
    put64(body, frames_done(ctx));
    put64(body + 8, ctx->total_cycles);
    body[16] = flags();
    body += 17;
    for (size_t index = 0; index < g_watch_count; index++) {
        for (uint32_t offset = 0; offset < g_watches[index].length; offset++) {
            *body++ = gbrt_peek8(ctx, (uint16_t)(g_watches[index].address + offset));
        }
    }
    return send_message();
}

static bool send_closing(GBContext* ctx) {
    uint8_t* body = message('X', 16);
    if (!body) return false;
    put64(body, frames_done(ctx));
    put64(body + 8, ctx->total_cycles);
    return send_message();
}

static bool set_watches(const uint8_t* body, size_t length) {
    size_t count = length / 4, total = 0;
    if (length % 4) return reply('E', "w takes an address and a length in two bytes each, any number of times");
    for (size_t index = 0; index < count; index++) total += get16(body + index * 4 + 2);
    if (total > MAX_WATCHED) return reply('E', "more than a megabyte to watch");
    Watch* watches = count ? (Watch*)malloc(count * sizeof(Watch)) : NULL;
    if (count && !watches) return reply('E', "out of memory");
    for (size_t index = 0; index < count; index++) {
        watches[index].address = get16(body + index * 4);
        watches[index].length = get16(body + index * 4 + 2);
    }
    free(g_watches);
    g_watches = watches;
    g_watch_count = count;
    g_watched_bytes = total;
    return reply('K', NULL);
}

static bool read_memory(GBContext* ctx, const uint8_t* body, size_t length) {
    if (length != 6) return reply('E', "r takes an address in two bytes and a length in four");
    uint32_t address = get16(body), count = get32(body + 2);
    if (address + (uint64_t)count > 0x10000u) return reply('E', "a read past $FFFF");
    uint8_t* out = message('M', count);
    if (!out) return false;
    for (uint32_t offset = 0; offset < count; offset++) out[offset] = gbrt_peek8(ctx, (uint16_t)(address + offset));
    return send_message();
}

/* A run is the buttons and nothing else: what a record does not hold is
 * not done while one is recorded or replayed. */
static const char* outside_a_run(const char* what) {
    static char text[96];
    if (!gbrt_run_recording() && !gbrt_run_replaying()) return NULL;
    snprintf(text, sizeof(text), "%s while a run is %s: it would not be in the record", what,
             gbrt_run_recording() ? "recorded" : "replayed");
    return text;
}

static bool write_memory(GBContext* ctx, const uint8_t* body, size_t length) {
    if (length < 2) return reply('E', "p takes an address in two bytes and the bytes to write");
    const char* refused = outside_a_run("no byte is written");
    if (refused) return reply('E', refused);
    uint32_t address = get16(body);
    size_t count = length - 2;
    if (address + (uint64_t)count > 0x10000u) return reply('E', "a write past $FFFF");
    for (size_t offset = 0; offset < count; offset++) {
        if (!gbrt_ram_byte(ctx, (uint16_t)(address + offset))) {
            return reply('E', "p writes RAM only: video memory, cartridge memory, work RAM, high RAM");
        }
    }
    for (size_t offset = 0; offset < count; offset++) {
        *gbrt_ram_byte(ctx, (uint16_t)(address + offset)) = body[2 + offset];
    }
    return reply('K', NULL);
}

static bool state_file(GBContext* ctx, bool load, const uint8_t* body, size_t length) {
    char path[1024];
    if (length == 0 || length >= sizeof(path)) return reply('E', "a path of 1 to 1023 bytes");
    memcpy(path, body, length);
    path[length] = '\0';
    if (load) {
        const char* refused = outside_a_run("no state is loaded");
        if (refused) return reply('E', refused);
        if (!gb_context_load_state_file(ctx, path)) return reply('E', "the state could not be loaded");
    } else if (!gb_context_save_state_file(ctx, path)) {
        return reply('E', "the state could not be written");
    }
    return reply('K', NULL);
}

static void held(uint8_t* dpad, uint8_t* buttons) {
    *buttons = (uint8_t)(0xF0u | (~g_held & 0x0Fu));
    *dpad = (uint8_t)(0xF0u | (~(g_held >> 4) & 0x0Fu));
}

bool gbrt_control_start(GBContext* ctx) {
    (void)ctx;
    if (!g_port) return true;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[CONTROL] no network on this machine\n");
        return false;
    }
#endif
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == BAD_SOCKET) {
        fprintf(stderr, "[CONTROL] cannot create a socket\n");
        return false;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)g_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g_port > 65535u || connect(g_socket, (struct sockaddr*)&address, sizeof(address)) != 0) {
        fprintf(stderr, "[CONTROL] nothing is listening on 127.0.0.1:%u for --control\n", g_port);
        drop();
        return false;
    }
    int one = 1;
    setsockopt(g_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(g_socket, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof(one));
#endif
    uint8_t* body = message('H', 6);
    if (!body) return false;
    put32(body, GBRT_CONTROL_VERSION);
    body[4] = 'g';
    body[5] = 'b';
    if (!send_message()) return false;
    g_started = true;
    fprintf(stderr, "[CONTROL] played by the program on 127.0.0.1:%u\n", g_port);
    return true;
}

bool gbrt_control_frame(GBContext* ctx, uint8_t* dpad, uint8_t* buttons) {
    held(dpad, buttons);
    if (!g_started) return true;
    if (g_lost || g_closing) return false;
    if (g_frames_left > 0 && --g_frames_left > 0) return true;
    if (!send_frame(ctx)) return false;
    for (;;) {
        uint8_t head[5];
        if (!receive(ctx, head, sizeof(head))) return false;
        size_t length = get32(head + 1);
        if (length > MAX_MESSAGE) {
            lose("the program sent a message of more than a megabyte");
            return false;
        }
        if (length + 1 > g_in_capacity) {
            uint8_t* grown = (uint8_t*)realloc(g_in, length + 1);
            if (!grown) {
                lose("out of memory");
                return false;
            }
            g_in = grown;
            g_in_capacity = length + 1;
        }
        if (length && !receive(ctx, g_in, length)) return false;
        bool sent = true;
        switch (head[0]) {
            case 'w': sent = set_watches(g_in, length); break;
            case 'r': sent = read_memory(ctx, g_in, length); break;
            case 'p': sent = write_memory(ctx, g_in, length); break;
            case 's': sent = state_file(ctx, false, g_in, length); break;
            case 'l': sent = state_file(ctx, true, g_in, length); break;
            case 'q': sent = send_frame(ctx); break;
            case 'g':
                if (length != 5 || get32(g_in + 1) == 0) {
                    sent = reply('E', "g takes the buttons in one byte and a number of frames, 1 or more, in four");
                    break;
                }
                g_held = g_in[0];
                g_frames_left = get32(g_in + 1);
                held(dpad, buttons);
                return true;
            case 'e':
                g_closing = true;
                return false;
            default:
                sent = reply('E', "no such message");
                break;
        }
        if (!sent) return false;
    }
}

bool gbrt_control_finish(GBContext* ctx) {
    if (!g_started) return true;
    if (g_socket != BAD_SOCKET) send_closing(ctx);
    drop();
    free(g_watches);
    free(g_out);
    free(g_in);
    g_watches = NULL;
    g_out = g_in = NULL;
    g_watch_count = g_watched_bytes = g_out_capacity = g_in_capacity = 0;
    g_started = false;
    return !g_lost;
}
