/*
 * Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * TMEM loads: LOAD_TILE, LOAD_BLOCK and LOAD_TLUT.
 *
 * Ported from parallel-rdp (commit 388d70f5835b352d841d9d9e5a08c5de01470f41)
 * rdp_renderer.cpp (Renderer::load_tile and load_tile_iteration, which turn
 * a load into upload descriptors) and shaders/tmem_update.comp, which
 * applies a descriptor to TMEM. The shader works as a gather: for each
 * 16-bit TMEM word it works out which RDRAM bytes the load would have put
 * there, if any. That formulation carries the hardware's quirks (odd-line
 * word swaps, LOAD_BLOCK's dxt-driven swizzle, the mismatched texel-size
 * cases, the 32-bit and YUV split between the TMEM halves, the four-fold
 * TLUT replication), so it is kept as is: each load runs it over all 2048
 * words.
 *
 * Which RDRAM bytes land in which TMEM word depends on the load's
 * parameters but not on the texture's address (but for its lowest bit) or
 * its contents, and games repeat the same few loads all the time. So the
 * gather's answer is remembered per set of parameters as a plan - a list of
 * (TMEM word, RDRAM offsets of its two bytes) - and a repeated load copies
 * the bytes the plan names instead of asking all 2048 words again.
 *
 * The shader's float reciprocals stand in for integer division and are
 * documented as exact for every input that reaches them; integer division
 * is used here, which gives the same quotients without depending on host
 * floating point.
 */
#include "n64_rdp_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct RdpUploadInfo {
    int32_t width, height;
    /* Divisors for the LOAD_BLOCK T bounds (1 / min_t_mod, 1 / max_t_mod). */
    int32_t min_t_div, max_t_div;
    int32_t vram_addr;
    int32_t vram_width;
    int32_t vram_size;
    int32_t vram_effective_width;
    int32_t tmem_offset;
    int32_t tmem_stride_words;
    int32_t tmem_size;
    int32_t tmem_fmt;
    int32_t mode;
    int32_t dxt;
} RdpUploadInfo;

/* compute_upload_t: floor(offset / divisor). Every divisor that reaches
 * here is at least 1 (a line holds at least one 64-bit word); the guard
 * only keeps a malformed state from dividing by zero. */
static int32_t upload_t(int32_t offset, int32_t divisor)
{
    if (divisor <= 0)
        return 0;
    return offset >= 0 ? offset / divisor : -((-offset + divisor - 1) / divisor);
}

/* Where one 16-bit TMEM word comes from: its high and low bytes are the
 * RDRAM bytes at addr + hi and addr + lo. */
typedef struct RdpTmemWord {
    int32_t addr;
    uint8_t hi, lo;
    int dirty;
} RdpTmemWord;

/* 32-bit and YUV loads read 64 bits and split them over the low and high
 * halves of TMEM. */
static void update_tmem_32(const RdpUploadInfo *info, int32_t tmem16_index, int upper_tmem, int yuv, RdpTmemWord *out)
{
    int32_t tmem16_offset = (info->tmem_offset & 0x7ff) >> 1;
    int32_t tmem16_stride = info->tmem_stride_words;
    int32_t pixel_offset = (tmem16_index - tmem16_offset) & 0x3ff;
    int32_t upload_x, upload_y, upload_x_xor = 0, iteration_offset = 0, last_line_upload_x;
    int32_t line_rdram_addr, rdram_addr;

    if (info->mode == RDP_UPLOAD_BLOCK) {
        int32_t word_offset = pixel_offset >> 1;
        if (info->tmem_stride_words == 0) {
            /* T comes straight from the iteration; a skewed dxt can write
             * a word twice or not at all. */
            int32_t first = word_offset & ~1;
            int32_t second = first + 1;
            int32_t first_t = rdp_asr(first * info->dxt, 16);
            int32_t second_t = rdp_asr(second * info->dxt, 16);
            if (first_t != second_t) {
                int32_t first_write = first ^ (first_t & 1);
                int32_t second_write = second ^ (second_t & 1);
                if (second_write == word_offset)
                    upload_x_xor = (second_t & 1) << 1;
                else if (first_write == word_offset)
                    upload_x_xor = (first_t & 1) << 1;
                else
                    return;
            } else {
                upload_x_xor ^= (first_t & 1) << 1;
            }
        } else {
            /* Solve word = floor(x * dt / 2048) * stride + x for x. */
            int32_t min_t = upload_t(word_offset & ~1, info->min_t_div);
            int32_t max_t = upload_t(word_offset | 1, info->max_t_div);
            int32_t max_word_candidate = (word_offset | 1) - tmem16_stride * min_t;
            int32_t min_word_candidate = (word_offset & ~1) - tmem16_stride * max_t;
            int found = 0;
            int32_t t;
            min_t = rdp_max(min_t, rdp_asr(min_word_candidate * info->dxt, 16));
            max_t = rdp_min(max_t, rdp_asr(max_word_candidate * info->dxt, 16));
            for (t = max_t; t >= min_t; t--) {
                int32_t first = (word_offset & ~1) - tmem16_stride * t;
                int32_t second = (word_offset | 1) - tmem16_stride * t;
                int32_t t_first = rdp_asr(first * info->dxt, 16);
                int32_t t_second = rdp_asr(second * info->dxt, 16);
                if (((second + t_second * tmem16_stride) ^ (t_second & 1)) == word_offset) {
                    found = 1;
                    pixel_offset = (second << 1) + (pixel_offset & 1);
                    break;
                } else if (((first + t_first * tmem16_stride) ^ (t_first & 1)) == word_offset) {
                    found = 1;
                    pixel_offset = (first << 1) + (pixel_offset & 1);
                    break;
                }
            }
            if (!found)
                return;
        }
        upload_x = pixel_offset;
        upload_y = 0;
    } else if (tmem16_stride == 0) {
        /* Every line lands on the same words; the last one is seen. */
        upload_x = pixel_offset;
        upload_y = info->height - 1;
    } else {
        upload_y = upload_t(pixel_offset, tmem16_stride);
        upload_x = pixel_offset - upload_y * tmem16_stride;
        /* A stride below the width: unroll into the last line. */
        if (upload_y >= info->height) {
            upload_x += tmem16_stride * (upload_y - info->height + 1);
            upload_y = info->height - 1;
        }
    }

    last_line_upload_x = upload_x ^ ((upload_y & 1) << 1);
    if (last_line_upload_x >= info->width && upload_y > 0) {
        /* The last line does not reach this word; the one before might. */
        upload_y--;
        upload_x += tmem16_stride;
    }

    upload_x ^= ((upload_y & 1) << 1) | upload_x_xor;

    if (info->vram_size == 3 || yuv) {
        iteration_offset = 4 * (upload_x & ~1);
    } else if (info->vram_size == 2) {
        /* 16-bit texels step four at a time, forming two 32-bit texels, and
         * a 32-bit tile does not shift x to match. */
        if ((upload_x & 2) != 0) {
            if (upload_y > 0) {
                upload_y--;
                upload_x += tmem16_stride;
                upload_x ^= 2;
            } else {
                return;
            }
        }
        iteration_offset = 2 * (upload_x & ~1);
    } else if (info->vram_size == 1) {
        int i;
        for (i = 0; i < 4 && upload_y > 0 && (upload_x & 6) != 0; i++) {
            upload_y--;
            upload_x += tmem16_stride;
            upload_x ^= 2;
        }
        if ((upload_x & 6) != 0)
            return;
        iteration_offset = upload_x & ~1;
    }

    if (upload_x >= info->width)
        return;

    line_rdram_addr = info->vram_addr + rdp_shl(upload_y * info->vram_width, info->vram_size - 1);
    rdram_addr = line_rdram_addr + iteration_offset + 4 * (upload_x & 1);
    /* Of the four bytes there: the low half of TMEM takes the first two and
     * the high half the last two, or for YUV the low half takes the
     * interleaved UV (bytes 0 and 2) and the high half Y (bytes 1 and 3). */
    out->addr = rdram_addr;
    if (yuv) {
        out->hi = upper_tmem ? 1 : 0;
        out->lo = upper_tmem ? 3 : 2;
    } else {
        out->hi = upper_tmem ? 2 : 0;
        out->lo = upper_tmem ? 3 : 1;
    }
    out->dirty = 1;
}

static void update_tmem_16(const RdpUploadInfo *info, int32_t tmem16_index, RdpTmemWord *out)
{
    int32_t tmem16_offset = (info->tmem_offset & 0xfff) >> 1;
    int32_t tmem16_stride = info->tmem_stride_words;
    int32_t pixel_offset = (tmem16_index - tmem16_offset) & 0x7ff;
    int32_t upload_x, upload_y, upload_x_xor = 0, iteration_offset = 0;
    int32_t line_rdram_addr, rdram_addr;

    if (info->mode == RDP_UPLOAD_BLOCK) {
        int32_t word_offset = pixel_offset >> 2;
        if (info->tmem_stride_words == 0) {
            upload_x_xor = (rdp_asr(word_offset * info->dxt, 16) & 1) << 1;
        } else {
            int32_t min_t = upload_t(word_offset, info->min_t_div);
            int32_t max_t = upload_t(word_offset, info->max_t_div);
            int32_t max_word_candidate = word_offset - tmem16_stride * min_t;
            int32_t min_word_candidate = word_offset - tmem16_stride * max_t;
            int found = 0;
            int32_t t;
            min_t = rdp_max(min_t, rdp_asr(min_word_candidate * info->dxt, 16));
            max_t = rdp_min(max_t, rdp_asr(max_word_candidate * info->dxt, 16));
            /* No break: the last (lowest t) solution wins, as in the
             * shader. */
            for (t = max_t; t >= min_t; t--) {
                int32_t candidate = word_offset - tmem16_stride * t;
                int32_t computed_t = rdp_asr(candidate * info->dxt, 16);
                if (candidate + computed_t * tmem16_stride == word_offset) {
                    found = 1;
                    upload_x_xor = (computed_t & 1) << 1;
                    pixel_offset = (candidate << 2) + (pixel_offset & 3);
                }
            }
            if (!found)
                return;
        }
        upload_x = pixel_offset;
        upload_y = 0;
    } else if (tmem16_stride == 0) {
        upload_x = pixel_offset;
        upload_y = info->height - 1;
    } else {
        upload_y = upload_t(pixel_offset, tmem16_stride);
        upload_x = pixel_offset - upload_y * tmem16_stride;
        if (upload_y >= info->height) {
            upload_x += tmem16_stride * (upload_y - info->height + 1);
            upload_y = info->height - 1;
        }
    }

    /* A tile texel size that differs from the image's: work out which
     * write, if any, lands on this word last. */
    if (info->tmem_size != info->vram_size) {
        if (info->vram_size - info->tmem_size == 1) {
            /* N-bit TMEM from 2N-bit RDRAM: mirrored writes; pick the half
             * of the 2N-bit load that is seen. */
            iteration_offset = (upload_x & ~3) * 4;
            if ((upload_x & ~3) + 2 < (info->vram_effective_width >> (3 - info->vram_size)))
                iteration_offset += 8;
        } else if (info->tmem_size == 2 && info->vram_size == 1) {
            /* 8-bit texels step eight at a time, forming four 16-bit
             * texels, and a 16-bit tile does not shift x to match. */
            if ((upload_x & 4) != 0) {
                if ((tmem16_stride & 4) != 0 && upload_y > 0) {
                    upload_y--;
                    upload_x += tmem16_stride;
                } else {
                    return;
                }
            }
            iteration_offset = upload_x & ~3;
        }
    } else {
        iteration_offset = (upload_x & ~3) * 2;
    }

    if (upload_x >= info->width)
        return;

    line_rdram_addr = info->vram_addr + rdp_shl(upload_y * info->vram_width, info->vram_size - 1);
    upload_x ^= ((upload_y & 1) << 1) | upload_x_xor;
    rdram_addr = line_rdram_addr + iteration_offset + 2 * (upload_x & 3);
    out->addr = rdram_addr;
    out->hi = 0;
    out->lo = 1;
    out->dirty = 1;
}

static void update_tmem_lut(const RdpUploadInfo *info, int32_t tmem16_index, RdpTmemWord *out)
{
    int32_t tmem16_offset = (info->tmem_offset & 0xfff) >> 1;
    int32_t pixel_offset = (tmem16_index - tmem16_offset) & 0x7ff;
    int32_t splat;
    int32_t rdram_addr;
    int32_t diff = info->vram_size - info->tmem_size;

    if (diff == 2) {
        splat = pixel_offset >> 2;
        splat <<= info->vram_size - 2;
        if (splat >= info->vram_effective_width)
            return;
    } else if (diff == 1) {
        int32_t shamt;
        if ((pixel_offset & 4) != 0)
            return;
        shamt = info->tmem_size + (info->vram_size == 2 ? 2 : 0);
        splat = (pixel_offset & ~7) >> shamt;
        if (splat >= info->vram_effective_width)
            return;
    } else if (diff == 0) {
        int32_t shamt;
        if ((pixel_offset & 0xc) != 0)
            return;
        shamt = info->tmem_size + (info->vram_size == 2 ? 2 : 0);
        splat = (pixel_offset & ~3) >> shamt;
        if (splat >= info->vram_effective_width)
            return;
    } else if (diff == -1) {
        if ((pixel_offset & 0x1c) != 0)
            return;
        splat = (pixel_offset >> info->tmem_size) & ~7;
        if (splat >= info->vram_effective_width)
            return;
    } else {
        /* 4-bit tile, 32-bit image: mirrored writes. */
        int32_t span_pixel = (pixel_offset >> 2) * 4;
        if (span_pixel + 2 < info->vram_effective_width)
            span_pixel += 2;
        if (span_pixel >= info->vram_effective_width)
            return;
        splat = span_pixel;
    }

    rdram_addr = info->vram_addr + rdp_shl(splat, info->vram_size - 1);
    /* Unaligned TLUT loads behave oddly. */
    rdram_addr += 2 * (rdram_addr & 1) * (pixel_offset & 3);
    out->addr = rdram_addr;
    out->hi = 0;
    out->lo = 1;
    out->dirty = 1;
}

/* Where TMEM word `index` comes from in the load `info` describes. */
static void tmem_source(const RdpUploadInfo *info, int32_t index, RdpTmemWord *w)
{
    int yuv = info->tmem_fmt == RDP_FMT_YUV;
    w->dirty = 0;
    if (info->mode == RDP_UPLOAD_TLUT)
        update_tmem_lut(info, index, w);
    else if (info->tmem_size == 3 || yuv)
        update_tmem_32(info, index & 0x3ff, index >= 0x400, yuv, w);
    else
        update_tmem_16(info, index, w);
}

/* The reference: ask every TMEM word. */
static void gather_upload(N64Rdp *rdp, const RdpUploadInfo *info)
{
    uint8_t *tmem = rdp->state->tmem;
    int32_t index;
    for (index = 0; index < 2048; index++) {
        RdpTmemWord w;
        tmem_source(info, index, &w);
        if (w.dirty) {
            tmem[2 * index] = n64_rdp_rdram_read8(rdp, (uint32_t)w.addr + w.hi);
            tmem[2 * index + 1] = n64_rdp_rdram_read8(rdp, (uint32_t)w.addr + w.lo);
        }
    }
}

/* A load's plan: the words it writes, with their sources relative to the
 * texture address less its lowest bit (which update_tmem_lut() looks at).
 * The key is the upload with only that bit for an address. */
typedef struct RdpPlanWord {
    uint16_t index;
    uint8_t hi, lo;
    int32_t offset;
} RdpPlanWord;

typedef struct RdpLoadPlan {
    RdpUploadInfo key;
    uint32_t count, capacity;
    RdpPlanWord *words; /* NULL: no plan in this slot */
} RdpLoadPlan;

#define RDP_LOAD_PLANS 256u

static RdpLoadPlan *find_plan(N64Rdp *rdp, const RdpUploadInfo *key)
{
    RdpLoadPlan *plans = (RdpLoadPlan *)rdp->links.load_plans, *plan;
    const uint8_t *bytes = (const uint8_t *)key;
    uint32_t hash = 2166136261u;
    size_t i;
    int32_t index;

    if (!plans) {
        plans = (RdpLoadPlan *)calloc(RDP_LOAD_PLANS, sizeof *plans);
        if (!plans)
            return NULL;
        rdp->links.load_plans = plans;
    }
    for (i = 0; i < sizeof *key; i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    plan = &plans[(hash ^ (hash >> 16)) % RDP_LOAD_PLANS];
    if (plan->words && memcmp(&plan->key, key, sizeof *key) == 0)
        return plan;

    /* Work the plan out, in place of whichever load had the slot. */
    plan->count = 0;
    for (index = 0; index < 2048; index++) {
        RdpTmemWord w;
        RdpPlanWord *word;
        tmem_source(key, index, &w);
        if (!w.dirty)
            continue;
        if (plan->count == plan->capacity) {
            uint32_t capacity = plan->capacity ? plan->capacity * 2u : 256u;
            RdpPlanWord *words = (RdpPlanWord *)realloc(plan->words, capacity * sizeof *words);
            if (!words) {
                free(plan->words);
                memset(plan, 0, sizeof *plan);
                return NULL;
            }
            plan->words = words;
            plan->capacity = capacity;
        }
        word = &plan->words[plan->count++];
        word->index = (uint16_t)index;
        word->hi = w.hi;
        word->lo = w.lo;
        word->offset = w.addr;
    }
    if (!plan->words) {
        /* A load that writes nothing still has a plan. */
        plan->words = (RdpPlanWord *)malloc(sizeof *plan->words);
        if (!plan->words)
            return NULL;
        plan->capacity = 1;
    }
    plan->key = *key;
    return plan;
}

void n64_rdp_free_load_plans(N64Rdp *rdp)
{
    RdpLoadPlan *plans = (RdpLoadPlan *)rdp->links.load_plans;
    uint32_t i;
    if (!plans)
        return;
    for (i = 0; i < RDP_LOAD_PLANS; i++)
        free(plans[i].words);
    free(plans);
    rdp->links.load_plans = NULL;
}

static void apply_upload(N64Rdp *rdp, const RdpUploadInfo *info)
{
    uint8_t *tmem = rdp->state->tmem;
    RdpUploadInfo key = *info;
    const RdpLoadPlan *plan;
    uint32_t base, i;

    if (n64_rdp_reference_path()) {
        gather_upload(rdp, info);
        return;
    }
    key.vram_addr = info->vram_addr & 1;
    plan = find_plan(rdp, &key);
    if (!plan) {
        gather_upload(rdp, info);
        return;
    }
    base = (uint32_t)info->vram_addr - (uint32_t)key.vram_addr;
    for (i = 0; i < plan->count; i++) {
        const RdpPlanWord *word = &plan->words[i];
        uint32_t addr = base + (uint32_t)word->offset;
        tmem[2 * word->index] = n64_rdp_rdram_read8(rdp, addr + word->hi);
        tmem[2 * word->index + 1] = n64_rdp_rdram_read8(rdp, addr + word->lo);
    }
}

/* Renderer::load_tile_iteration. */
static void load_tile_iteration(N64Rdp *rdp, uint32_t tile, const RdpLoadInfo *info, uint32_t tmem_offset)
{
    N64RdpTile *t = &rdp->state->tiles[tile];
    RdpUploadInfo upload;
    uint32_t upload_x = 0, upload_y = 0;
    int upload_mode = info->mode;

    t->slo = info->slo;
    t->shi = info->shi;
    t->tlo = info->tlo;
    t->thi = info->thi;

    /* Combinations the hardware does not support (or that crash it), which
     * parallel-rdp ignores after updating the tile size. */
    if (t->fmt == RDP_FMT_YUV && (t->size != RDP_SIZE_16 || info->size != RDP_SIZE_16))
        return;
    if (info->size == RDP_SIZE_4)
        return;
    if (t->size == RDP_SIZE_32 && t->fmt != RDP_FMT_RGBA)
        return;
    if (info->mode == RDP_UPLOAD_TLUT && t->size == RDP_SIZE_32)
        return;
    if (info->mode != RDP_UPLOAD_TLUT) {
        if (info->size == RDP_SIZE_32 && t->size == RDP_SIZE_8)
            return;
        if (info->size == RDP_SIZE_16 && t->size == RDP_SIZE_4)
            return;
        if (info->size == RDP_SIZE_32 && t->size == RDP_SIZE_4)
            return;
    }

    memset(&upload, 0, sizeof upload);
    upload.tmem_stride_words = (int32_t)(t->stride >> 1);

    if (upload_mode == RDP_UPLOAD_BLOCK) {
        /* LOAD_BLOCK gives a width and dxt rather than a height: T advances
         * by dxt per 64-bit word, and odd T swaps 32-bit words. */
        uint32_t pixel_count = (info->shi - info->slo + 1) & 0xfff;
        uint32_t dt = info->thi;
        uint32_t max_tmem_iteration = (pixel_count - 1) >> (4u - info->size);
        uint32_t max_t = (max_tmem_iteration * dt) >> 11;

        upload_x = info->slo;
        upload_y = info->tlo;

        if (max_t != 0) {
            uint32_t max_num = ((1u << 11) + dt - 1u) / dt;
            uint32_t min_num = (1u << 11) / dt;
            int uneven_dt = max_num != min_num;
            uint32_t max_line_stride, min_line_stride;

            if (uneven_dt) {
                /* If the accumulated error stays below one step, every line
                 * has the larger count. */
                uint32_t overflow_amt = dt * max_num - (1u << 11);
                overflow_amt *= max_t;
                if (overflow_amt < dt) {
                    min_num = max_num;
                    uneven_dt = 0;
                }
            }

            upload.dxt = (int32_t)(dt << 5);
            if (t->size == RDP_SIZE_32 || t->fmt == RDP_FMT_YUV) {
                /* Two iterations make one 64-bit word. */
                upload.tmem_stride_words <<= 1;
                if (uneven_dt && t->size != info->size)
                    return;
            }
            if (t->size > info->size) {
                uint32_t shamt = (uint32_t)t->size - info->size;
                max_num <<= shamt;
                min_num <<= shamt;
                upload.dxt >>= shamt;
            } else if (info->size > t->size) {
                /* parallel-rdp does not support a texel size larger in
                 * RDRAM than in the tile for LOAD_BLOCK. */
                return;
            }

            max_line_stride = max_num + (uint32_t)(upload.tmem_stride_words >> 2);
            min_line_stride = min_num + (uint32_t)(upload.tmem_stride_words >> 2);
            upload.min_t_div = (int32_t)max_line_stride;
            upload.max_t_div = (int32_t)min_line_stride;
            upload.width = (int32_t)pixel_count;
            upload.height = 1;
            upload.tmem_stride_words >>= 2; /* in 64-bit words */
        } else {
            /* T never leaves zero, so this is a one-line tile load. */
            upload.width = (int32_t)pixel_count;
            upload.height = 1;
            upload.tmem_stride_words = 0;
            upload_mode = RDP_UPLOAD_TILE;
        }
    } else {
        upload_x = info->slo >> 2;
        upload_y = info->tlo >> 2;
        upload.width = (int32_t)((((info->shi >> 2) - (info->slo >> 2)) + 1) & 0xfff);
        upload.height = (int32_t)(((info->thi >> 2) - (info->tlo >> 2)) + 1);
    }

    if (!upload.width)
        return;

    switch (info->size) {
    case RDP_SIZE_8: upload.vram_effective_width = (upload.width + 7) & ~7; break;
    case RDP_SIZE_16:
        /* A 16-bit TLUT load steps one texel at a time, not four. */
        upload.vram_effective_width = upload_mode == RDP_UPLOAD_TLUT ? upload.width : (upload.width + 3) & ~3;
        break;
    case RDP_SIZE_32: upload.vram_effective_width = (upload.width + 1) & ~1; break;
    default: break;
    }

    /* Loads move 64 bits at a time; width becomes 16-bit TMEM words. */
    switch (t->size) {
    case RDP_SIZE_4: upload.width = ((upload.width + 15) & ~15) >> 2; break;
    case RDP_SIZE_8: upload.width = ((upload.width + 7) & ~7) >> 1; break;
    case RDP_SIZE_16:
        upload.width = (upload.width + 3) & ~3;
        if (t->fmt == RDP_FMT_YUV)
            upload.width >>= 1; /* YUV loads like 32-bit */
        break;
    default: upload.width = (upload.width + 1) & ~1; break;
    }

    if (upload.height > 1 && upload_mode == RDP_UPLOAD_TLUT)
        return;

    upload.vram_addr = (int32_t)(info->tex_addr + ((info->tex_width * upload_y + upload_x) << (info->size - 1)));
    upload.vram_width = upload_mode == RDP_UPLOAD_BLOCK ? upload.vram_effective_width : (int32_t)info->tex_width;
    upload.vram_size = (int32_t)info->size;
    upload.tmem_offset = (int32_t)((t->offset + tmem_offset) & 0xfff);
    upload.tmem_size = t->size;
    upload.tmem_fmt = t->fmt;
    upload.mode = upload_mode;

    apply_upload(rdp, &upload);
}

void n64_rdp_load_tile(N64Rdp *rdp, uint32_t tile, const RdpLoadInfo *info)
{
    N64RdpTile *t = &rdp->state->tiles[tile];

    /* Loads that move nothing. */
    if (info->mode != RDP_UPLOAD_BLOCK) {
        if ((info->thi >> 2) < (info->tlo >> 2))
            return;
        if (((((info->shi >> 2) - (info->slo >> 2)) + 1) & 0xfff) == 0)
            return;
    } else {
        uint32_t pixel_count = ((info->shi - info->slo) + 1) & 0xfff;
        if (!pixel_count || pixel_count > 2048)
            return;
    }

    if (info->mode == RDP_UPLOAD_TILE) {
        uint32_t pixels_per_line = (((info->shi >> 2) - (info->slo >> 2)) + 1) & 0xfff;
        /* 32-bit and YUV loads split into two halves, so they wrap at 2 KiB
         * instead of 4. */
        uint32_t quad_words_per_line = ((pixels_per_line << t->size) + 15) >> 4;
        uint32_t bytes_per_line, max_bytes, num_lines;

        if (t->size > info->size)
            quad_words_per_line <<= (uint32_t)t->size - info->size;
        else if (t->size < info->size)
            quad_words_per_line >>= info->size - (uint32_t)t->size;

        bytes_per_line = quad_words_per_line * 8 > t->stride ? quad_words_per_line * 8 : t->stride;
        max_bytes = t->fmt == RDP_FMT_YUV ? 0x800u : 0x1000u;
        num_lines = ((info->thi >> 2) - (info->tlo >> 2)) + 1;

        if (bytes_per_line * num_lines > max_bytes) {
            /* More than TMEM's wraparound in one load: split it so each
             * part's gather sees one wrap. */
            uint32_t max_lines = (max_bytes / bytes_per_line) & ~1u;
            uint32_t line;
            if (max_lines == 0)
                return; /* parallel-rdp rejects a single line over 2 KiB */
            for (line = 0; line < num_lines; line += max_lines) {
                uint32_t to_copy = num_lines - line < max_lines ? num_lines - line : max_lines;
                RdpLoadInfo part = *info;
                part.tlo = info->tlo + (line << 2);
                part.thi = part.tlo + ((to_copy - 1) << 2);
                load_tile_iteration(rdp, tile, &part, line * t->stride);
            }
            t->slo = info->slo;
            t->shi = info->shi;
            t->tlo = info->tlo;
            t->thi = info->thi;
            return;
        }
    }
    load_tile_iteration(rdp, tile, info, 0);
}
