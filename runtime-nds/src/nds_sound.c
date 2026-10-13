/*
 * Sound: where the machine meets the sound unit (nds_spu.c). The unit owns
 * its registers and the mixing; this file gives it the ARM7's memory to
 * fetch samples from and store captures in, calls it once every mixer
 * period, and keeps what it produces for a frontend to collect.
 */
#include "nds.h"

#include <string.h>

#include "nds_prof.h"

static uint32_t host_read(void *user, uint32_t addr, int size)
{
    (void)size; /* the unit only ever asks for aligned words */
    return nds7_device_read32((NdsMachine *)user, addr);
}

static void host_write(void *user, uint32_t addr, uint32_t value, int size)
{
    (void)size;
    nds7_device_write32((NdsMachine *)user, addr, value);
}

static NdsSpuHost host_for(NdsMachine *m)
{
    NdsSpuHost host = { host_read, host_write, m };
    return host;
}

void nds_sound_reset(NdsMachine *m)
{
    nds_spu_reset(&m->spu);
    memset(m->audio_ring, 0, sizeof m->audio_ring);
    m->audio_written = m->audio_taken = 0;
    nds_schedule(m, NDS_EVENT_SOUND, NDS_CYCLES_PER_SAMPLE);
}

uint32_t nds_sound_read7(NdsMachine *m, uint32_t addr, int size)
{
    return nds_spu_read(&m->spu, addr, size);
}

void nds_sound_write7(NdsMachine *m, uint32_t addr, uint32_t value, int size)
{
    NdsSpuHost host = host_for(m);
    nds_spu_write(&m->spu, &host, addr, value, size);
}

/* One mixer period. */
void nds_sound_event(NdsMachine *m)
{
    NdsSpuHost host = host_for(m);
    int16_t frame[2];
    NDS_PROF_BEGIN(NDS_PROF_SOUND);
    nds_spu_sample(&m->spu, &host, frame);
    NDS_PROF_END();
    /* POWCNT2 bit 0, the speakers: off silences the output; the unit runs
     * on behind it. */
    if (!(m->powcnt2 & 1u)) frame[0] = frame[1] = 0;

    if (m->audio_written - m->audio_taken == NDS_AUDIO_RING) m->audio_taken++; /* nobody is listening */
    int16_t *slot = m->audio_ring[m->audio_written % NDS_AUDIO_RING];
    slot[0] = frame[0];
    slot[1] = frame[1];
    m->audio_written++;

    nds_schedule(m, NDS_EVENT_SOUND, m->event_at[NDS_EVENT_SOUND] + NDS_CYCLES_PER_SAMPLE);
}

size_t nds_audio_read(NdsMachine *m, int16_t *out, size_t max_frames)
{
    size_t count = 0;
    while (count < max_frames && m->audio_taken != m->audio_written) {
        const int16_t *slot = m->audio_ring[m->audio_taken % NDS_AUDIO_RING];
        out[count * 2] = slot[0];
        out[count * 2 + 1] = slot[1];
        m->audio_taken++;
        count++;
    }
    return count;
}
