/*
 * Inter-processor communication (GBATEK, "DS Inter Process Communication
 * (IPC)"): IPCSYNC, a four-bit value each CPU shows the other with an
 * interrupt on request, and the IPC FIFO, sixteen words in each direction.
 *
 * The FIFO's two interrupts are edge triggered: IF.17 is set when "send
 * FIFO empty and its interrupt enabled" becomes true, IF.18 when "receive
 * FIFO not empty and its interrupt enabled" does. Every operation here
 * samples both conditions for both CPUs before and after, and raises what
 * rose.
 */
#include "nds.h"

/* Bit 0: send-empty condition. Bit 1: receive-not-empty condition. */
static unsigned conditions(const NdsMachine *m, int cpu)
{
    const NdsIpcFifo *send = &m->ipc_fifo[cpu];
    const NdsIpcFifo *recv = &m->ipc_fifo[cpu ^ 1];
    unsigned bits = 0;
    if ((m->ipcfifocnt[cpu] & 0x0004u) && send->count == 0) bits |= 1u;
    if ((m->ipcfifocnt[cpu] & 0x0400u) && recv->count != 0) bits |= 2u;
    return bits;
}

static void raise_edges(NdsMachine *m, const unsigned before[2])
{
    for (int cpu = 0; cpu < 2; cpu++) {
        unsigned rose = conditions(m, cpu) & ~before[cpu];
        if (rose & 1u) nds_raise_irq(m, cpu, NDS_IRQ_IPC_SEND_EMPTY);
        if (rose & 2u) nds_raise_irq(m, cpu, NDS_IRQ_IPC_RECV);
    }
}

/* IPCFIFOCNT as it reads: the stored enables and error bit, and the state
 * of both FIFOs. */
static uint16_t fifocnt(const NdsMachine *m, int cpu)
{
    const NdsIpcFifo *send = &m->ipc_fifo[cpu];
    const NdsIpcFifo *recv = &m->ipc_fifo[cpu ^ 1];
    uint16_t value = m->ipcfifocnt[cpu] & 0xC404u;
    if (send->count == 0) value |= 0x0001u;
    if (send->count == 16) value |= 0x0002u;
    if (recv->count == 0) value |= 0x0100u;
    if (recv->count == 16) value |= 0x0200u;
    return value;
}

static uint32_t read_register(const NdsMachine *m, int cpu, uint32_t reg)
{
    switch (reg) {
    case 0x180: /* IPCSYNC: our output and enable, and the other side's output */
        return (m->ipcsync[cpu] & 0x4F00u) | ((m->ipcsync[cpu ^ 1] >> 8) & 0x000Fu);
    case 0x184:
        return fifocnt(m, cpu);
    default:
        return 0; /* IPCFIFOSEND is write-only */
    }
}

uint32_t nds_ipc_read(NdsMachine *m, int cpu, uint32_t offset, int size)
{
    uint32_t value = read_register(m, cpu, offset & ~3u) >> ((offset & 3u) * 8u);
    return size == 4 ? value : size == 2 ? value & 0xFFFFu : value & 0xFFu;
}

static void send(NdsMachine *m, int cpu, uint32_t value)
{
    NdsIpcFifo *fifo = &m->ipc_fifo[cpu];
    if (!(m->ipcfifocnt[cpu] & 0x8000u)) return; /* disabled: ignored, no error */
    if (fifo->count == 16) {
        m->ipcfifocnt[cpu] |= 0x4000u;
        return;
    }
    unsigned before[2] = { conditions(m, 0), conditions(m, 1) };
    fifo->data[(fifo->read + fifo->count) & 15u] = value;
    fifo->count++;
    raise_edges(m, before);
}

void nds_ipc_write(NdsMachine *m, int cpu, uint32_t offset, uint32_t value, int size)
{
    uint32_t reg = offset & ~3u;
    uint32_t shift = (offset & 3u) * 8u;
    uint32_t mask = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << shift;
    value <<= shift;

    if (reg == 0x180) {
        m->ipcsync[cpu] = (uint16_t)((m->ipcsync[cpu] & ~mask) | (value & mask & 0x4F00u));
        /* Bit 13: interrupt the other CPU, if it has asked for that. */
        if ((value & mask & 0x2000u) && (m->ipcsync[cpu ^ 1] & 0x4000u)) {
            nds_raise_irq(m, cpu ^ 1, NDS_IRQ_IPC_SYNC);
        }
        return;
    }
    if (reg == 0x184) {
        unsigned before[2] = { conditions(m, 0), conditions(m, 1) };
        uint32_t writable = mask & 0x8404u;
        m->ipcfifocnt[cpu] = (uint16_t)((m->ipcfifocnt[cpu] & ~writable) | (value & writable));
        if (value & mask & 0x4000u) m->ipcfifocnt[cpu] &= 0xBFFFu; /* acknowledge the error */
        if (value & mask & 0x0008u) {
            /* Flush our send FIFO; the other side then reads zero. */
            NdsIpcFifo *fifo = &m->ipc_fifo[cpu];
            fifo->read = fifo->count = 0;
            fifo->last = 0;
        }
        raise_edges(m, before);
        return;
    }
    if (reg == 0x188) {
        /* Narrower writes still send one word. */
        send(m, cpu, value & mask);
    }
}

uint32_t nds_ipc_recv(NdsMachine *m, int cpu)
{
    NdsIpcFifo *fifo = &m->ipc_fifo[cpu ^ 1];
    if (fifo->count == 0) {
        /* Empty: the last word again (or zero), and the error bit. */
        if (m->ipcfifocnt[cpu] & 0x8000u) m->ipcfifocnt[cpu] |= 0x4000u;
        return fifo->last;
    }
    uint32_t value = fifo->data[fifo->read];
    /* Disabled: the oldest word, left in the FIFO. */
    if (!(m->ipcfifocnt[cpu] & 0x8000u)) return value;

    unsigned before[2] = { conditions(m, 0), conditions(m, 1) };
    fifo->read = (uint8_t)((fifo->read + 1u) & 15u);
    fifo->count--;
    fifo->last = value;
    raise_edges(m, before);
    return value;
}
