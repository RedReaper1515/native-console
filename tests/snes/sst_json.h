/*
 * Minimal JSON reader for the SingleStepTests vector files (65816 and
 * SPC700). Parses a whole file into an arena of nodes; objects and arrays
 * link their children. Numbers are integers (the vectors have no
 * fractions). Fails closed: any syntax error rejects the file.
 */
#ifndef SST_JSON_H
#define SST_JSON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { JS_NULL, JS_NUM, JS_STR, JS_ARR, JS_OBJ, JS_BOOL } JsType;

typedef struct JsNode {
    JsType type;
    int64_t num;
    const char *str; /* strings and object keys point into the file buffer */
    uint32_t len;
    const char *key; /* when the node is an object member */
    uint32_t key_len;
    int32_t child, next; /* indices, -1 = none */
} JsNode;

typedef struct {
    char *buf;
    size_t size, pos;
    JsNode *nodes;
    size_t count, cap;
    bool error;
} JsDoc;

static int32_t js_new(JsDoc *d, JsType t)
{
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 1u << 16;
        JsNode *n = (JsNode *)realloc(d->nodes, d->cap * sizeof *n);
        if (!n) {
            d->error = true;
            return -1;
        }
        d->nodes = n;
    }
    JsNode *n = &d->nodes[d->count];
    memset(n, 0, sizeof *n);
    n->type = t;
    n->child = n->next = -1;
    return (int32_t)d->count++;
}

static void js_ws(JsDoc *d)
{
    while (d->pos < d->size) {
        char ch = d->buf[d->pos];
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
        d->pos++;
    }
}

static int32_t js_value(JsDoc *d);

static bool js_string(JsDoc *d, const char **s, uint32_t *len)
{
    if (d->pos >= d->size || d->buf[d->pos] != '"') return false;
    size_t start = ++d->pos;
    while (d->pos < d->size && d->buf[d->pos] != '"') {
        if (d->buf[d->pos] == '\\') d->pos++;
        d->pos++;
    }
    if (d->pos >= d->size) return false;
    *s = d->buf + start;
    *len = (uint32_t)(d->pos - start);
    d->pos++;
    return true;
}

static int32_t js_value(JsDoc *d)
{
    js_ws(d);
    if (d->pos >= d->size) {
        d->error = true;
        return -1;
    }
    char ch = d->buf[d->pos];
    if (ch == '[' || ch == '{') {
        const bool obj = ch == '{';
        int32_t self = js_new(d, obj ? JS_OBJ : JS_ARR), last = -1;
        if (self < 0) return -1;
        d->pos++;
        js_ws(d);
        if (d->pos < d->size && d->buf[d->pos] == (obj ? '}' : ']')) {
            d->pos++;
            return self;
        }
        for (;;) {
            const char *key = NULL;
            uint32_t key_len = 0;
            if (obj) {
                js_ws(d);
                if (!js_string(d, &key, &key_len)) break;
                js_ws(d);
                if (d->pos >= d->size || d->buf[d->pos] != ':') break;
                d->pos++;
            }
            int32_t v = js_value(d);
            if (v < 0) break;
            d->nodes[v].key = key;
            d->nodes[v].key_len = key_len;
            if (last < 0) d->nodes[self].child = v;
            else d->nodes[last].next = v;
            last = v;
            js_ws(d);
            if (d->pos < d->size && d->buf[d->pos] == ',') {
                d->pos++;
                continue;
            }
            if (d->pos < d->size && d->buf[d->pos] == (obj ? '}' : ']')) {
                d->pos++;
                return self;
            }
            break;
        }
        d->error = true;
        return -1;
    }
    if (ch == '"') {
        int32_t n = js_new(d, JS_STR);
        if (n < 0 || !js_string(d, &d->nodes[n].str, &d->nodes[n].len)) d->error = true;
        return d->error ? -1 : n;
    }
    if (ch == 'n' && d->pos + 4 <= d->size && !memcmp(d->buf + d->pos, "null", 4)) {
        d->pos += 4;
        return js_new(d, JS_NULL);
    }
    if (ch == 't' && d->pos + 4 <= d->size && !memcmp(d->buf + d->pos, "true", 4)) {
        d->pos += 4;
        int32_t n = js_new(d, JS_BOOL);
        if (n >= 0) d->nodes[n].num = 1;
        return n;
    }
    if (ch == 'f' && d->pos + 5 <= d->size && !memcmp(d->buf + d->pos, "false", 5)) {
        d->pos += 5;
        return js_new(d, JS_BOOL);
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        bool neg = ch == '-';
        if (neg) d->pos++;
        int64_t v = 0;
        size_t digits = 0;
        while (d->pos < d->size && d->buf[d->pos] >= '0' && d->buf[d->pos] <= '9') {
            v = v * 10 + (d->buf[d->pos++] - '0');
            digits++;
        }
        if (!digits) {
            d->error = true;
            return -1;
        }
        int32_t n = js_new(d, JS_NUM);
        if (n >= 0) d->nodes[n].num = neg ? -v : v;
        return n;
    }
    d->error = true;
    return -1;
}

/* Load and parse a file; returns the root node index or -1. */
static int32_t js_load(JsDoc *d, const char *path)
{
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d->buf = (char *)malloc(len > 0 ? (size_t)len : 1u);
    if (!d->buf || fread(d->buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    d->size = (size_t)len;
    int32_t root = js_value(d);
    return d->error ? -1 : root;
}

static void js_free(JsDoc *d)
{
    free(d->buf);
    free(d->nodes);
    memset(d, 0, sizeof *d);
}

static int32_t js_get(const JsDoc *d, int32_t obj, const char *key)
{
    size_t klen = strlen(key);
    for (int32_t n = obj >= 0 ? d->nodes[obj].child : -1; n >= 0; n = d->nodes[n].next) {
        if (d->nodes[n].key_len == klen && !memcmp(d->nodes[n].key, key, klen)) return n;
    }
    return -1;
}

static int64_t js_num(const JsDoc *d, int32_t obj, const char *key, bool *ok)
{
    int32_t n = js_get(d, obj, key);
    if (n < 0 || d->nodes[n].type != JS_NUM) {
        *ok = false;
        return 0;
    }
    return d->nodes[n].num;
}

#endif /* SST_JSON_H */
