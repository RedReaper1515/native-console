/*
 * The game card's ROM interface (GBATEK, "DS Cartridge I/O Ports" and "DS
 * Cartridge Protocol").
 *
 * A game is started with the card already in its main data mode, where only
 * two commands matter: B7 (read from an address) and B8 (the chip ID). The
 * program writes eight command bytes and then ROMCTRL with the start bit;
 * the card answers with a block of words that are read one at a time from
 * the data register, by hand or by a card-mode DMA. Each word takes time to
 * clock in, and the next one starts when the last was taken.
 *
 * The card's other face, its save memory on the AUXSPI port, is reached
 * through AUXSPICNT and AUXSPIDATA here.
 */
#include "nds.h"

/* System cycles to clock one byte: ROMCTRL bit 27 picks 33.51 MHz / 5 or
 * / 8. */
static uint32_t byte_cycles(uint32_t romctrl)
{
    return (romctrl & (1u << 27)) ? 8u : 5u;
}

static uint32_t next_word(NdsMachine *m)
{
    NdsCard *card = &m->card;
    if (card->reply == NDS_CARD_REPLY_CHIP_ID) return m->chip_id;
    if (card->reply != NDS_CARD_REPLY_DATA || !m->rom) return 0xFFFFFFFFu; /* high-Z */
    uint32_t word = nds_get_le32(m->rom + (card->address & m->rom_mask & ~3u));
    card->address += 4;
    return word;
}

/* The transfer is over: the busy and ready bits drop, and the card
 * interrupt is raised if AUXSPICNT asks for it. */
static void finish(NdsMachine *m)
{
    NdsCard *card = &m->card;
    int cpu = card->owner;
    card->romctrl[cpu] &= ~((1u << 31) | (1u << 23));
    card->word_ready = false;
    if (card->auxspicnt[cpu] & (1u << 14)) nds_raise_irq(m, cpu, NDS_IRQ_CARD_DONE);
}

/* A word has arrived in the data register. */
void nds_card_event(NdsMachine *m)
{
    NdsCard *card = &m->card;
    nds_schedule(m, NDS_EVENT_CARD, UINT64_MAX);
    if (!card->remaining) return;
    card->word = next_word(m);
    card->word_ready = true;
    card->romctrl[card->owner] |= 1u << 23;
    nds_dma_trigger(m, card->owner, NDS_DMA_CARD);
}

uint32_t nds_card_data(NdsMachine *m, int cpu)
{
    NdsCard *card = &m->card;
    if (!card->word_ready || cpu != card->owner) return 0;
    uint32_t word = card->word;
    card->word_ready = false;
    card->romctrl[cpu] &= ~(1u << 23);
    card->remaining = card->remaining > 4 ? card->remaining - 4 : 0;
    if (card->remaining) {
        /* The card clocks the next word in now that this one is taken. */
        nds_schedule(m, NDS_EVENT_CARD, nds_cpu_now(m, cpu) + card->word_cycles);
    } else {
        finish(m);
    }
    return word;
}

static void start(NdsMachine *m, int cpu)
{
    NdsCard *card = &m->card;
    const uint8_t *cmd = card->command[cpu];
    uint32_t romctrl = card->romctrl[cpu];

    uint32_t size_code = (romctrl >> 24) & 7u;
    uint32_t length = size_code == 0 ? 0u : size_code == 7 ? 4u : 0x100u << size_code;

    card->owner = (uint8_t)cpu;
    card->reply = NDS_CARD_REPLY_DATA;
    card->word_ready = false;
    card->remaining = length;
    card->address = 0;
    card->romctrl[cpu] &= ~(1u << 23);

    switch (cmd[0]) {
    case 0xB7: { /* read data */
        uint32_t address = ((uint32_t)cmd[1] << 24) | ((uint32_t)cmd[2] << 16) | ((uint32_t)cmd[3] << 8) | cmd[4];
        /* The header and secure area cannot be read this way: addresses
         * below 0x8000 read from 0x8000 on, repeating every 0x200. */
        if (address < 0x8000u) address = 0x8000u + (address & 0x1FFu);
        card->address = address;
        break;
    }
    case 0xB8: /* chip ID, repeated */
    case 0x90:
        card->reply = NDS_CARD_REPLY_CHIP_ID;
        break;
    case 0x00: /* the header, in the unencrypted mode a booted game never sees */
        card->address = 0;
        break;
    default:
        card->reply = NDS_CARD_REPLY_NOTHING;
        break;
    }

    if (!length) {
        finish(m);
        return;
    }
    /* Eight command bytes, the gap ROMCTRL asks for, then the first word. */
    uint32_t clk = byte_cycles(romctrl);
    card->word_cycles = 4u * clk;
    uint64_t delay = (uint64_t)(8u + (romctrl & 0x1FFFu)) * clk + card->word_cycles;
    nds_schedule(m, NDS_EVENT_CARD, nds_cpu_now(m, cpu) + delay);
}

static uint8_t read_byte(const NdsMachine *m, int cpu, uint32_t offset)
{
    const NdsCard *card = &m->card;
    if (offset < 0x1A2) return (uint8_t)((card->auxspicnt[cpu] & 0xE043u) >> ((offset - 0x1A0) * 8u));
    if (offset == 0x1A2) return card->auxspi_data;
    if (offset >= 0x1A4 && offset < 0x1A8) return (uint8_t)(card->romctrl[cpu] >> ((offset - 0x1A4) * 8u));
    if (offset >= 0x1A8 && offset < 0x1B0) return card->command[cpu][offset - 0x1A8];
    return 0;
}

uint32_t nds_card_read(NdsMachine *m, int cpu, uint32_t offset, int size)
{
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value |= (uint32_t)read_byte(m, cpu, offset + (uint32_t)i) << (8 * i);
    return value;
}

void nds_card_write(NdsMachine *m, int cpu, uint32_t offset, uint32_t value, int size)
{
    NdsCard *card = &m->card;
    bool go = false;
    for (int i = 0; i < size; i++) {
        uint32_t at = offset + (uint32_t)i;
        uint8_t byte = (uint8_t)(value >> (8 * i));
        /* The card slot belongs to one CPU at a time (EXMEMCNT bit 11: set
         * for the ARM7). The other one's port reaches nothing. */
        const bool owner = (int)((m->exmemcnt[NDS_ARM9] >> 11) & 1u) == cpu;
        if (at == 0x1A0) {
            card->auxspicnt[cpu] = (uint16_t)((card->auxspicnt[cpu] & 0xFF00u) | (byte & 0x43u));
        } else if (at == 0x1A1) {
            card->auxspicnt[cpu] = (uint16_t)((card->auxspicnt[cpu] & 0x00FFu) | ((byte & 0xE0u) << 8));
            /* Switching the port or its serial mode off releases the save
             * chip's select line, which ends its command. */
            if (owner && (card->auxspicnt[cpu] & 0xA000u) != 0xA000u) nds_save_deselect(&m->save);
        } else if (at == 0x1A2) {
            /* A byte to the save chip and one back, if the port is on and
             * in serial mode. With bit 6 (hold) clear, select is released
             * after the byte. */
            if (owner && (card->auxspicnt[cpu] & 0xA000u) == 0xA000u && m->save_data) {
                card->auxspi_data = nds_save_transfer(&m->save, m->save_data, byte, &m->save_dirty);
                if (!(card->auxspicnt[cpu] & 0x0040u)) nds_save_deselect(&m->save);
                /* A chip of unknown kind shows what it is by the commands
                 * it is sent. */
                if (m->save.type != NDS_SAVE_UNKNOWN) {
                    m->save_type = m->save.type;
                    m->save_size = m->save.size;
                }
            } else {
                card->auxspi_data = 0xFF;
            }
        } else if (at >= 0x1A4 && at < 0x1A8) {
            uint32_t shift = (at - 0x1A4) * 8u;
            /* Bit 23 is read-only, bit 29 cannot be cleared once set, and
             * bit 31 starts a transfer and stays set until it ends. */
            uint32_t writable = 0x5F7F7FFFu & (0xFFu << shift);
            uint32_t sticky = card->romctrl[cpu] & 0xA0000000u;
            card->romctrl[cpu] = (card->romctrl[cpu] & ~writable) | (((uint32_t)byte << shift) & writable) | sticky;
            if (at == 0x1A7) {
                card->romctrl[cpu] |= ((uint32_t)byte << 24) & 0x20000000u;
                if ((byte & 0x80u) && !(sticky & 0x80000000u)) go = true;
            }
        } else if (at >= 0x1A8 && at < 0x1B0) {
            card->command[cpu][at - 0x1A8] = byte;
        }
        /* 0x1B0-0x1BB: the encryption seeds, which nothing here uses. */
    }
    if (go) {
        card->romctrl[cpu] |= 1u << 31;
        start(m, cpu);
    }
}
