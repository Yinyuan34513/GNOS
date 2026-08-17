/*
 * audio.c — Intel 82801AA AC'97 audio driver. (GPLv2)
 *
 * See ac97.h for the shape of the device.  The interesting part of this file
 * is the Buffer Descriptor List: an array of up to 32 entries, each naming a
 * *physical* buffer and how many 16-bit samples are in it.  The engine plays
 * entry CIV ("current index value"), advances, and stops when it reaches the
 * entry software marked as LVI ("last valid index").  So the ring is not a
 * ring of bytes, it is a ring of buffers, and the two indices are the whole
 * flow-control protocol:
 *
 *      CIV ------------------> LVI
 *      |<-- card is playing -->|<-- software owns these -->|
 *
 * Because everything here is DMA, the addresses in the BDL must be physical
 * and the buffers must not move.  We take them straight from the page frame
 * allocator, which gives us both properties for free.
 *
 * Verification on a headless machine: with `-audiodev none` QEMU still runs
 * the engine off a real-time timer, so the play position genuinely advances.
 * The self-test therefore asserts something falsifiable -- that PICB, the
 * count of samples left in the current buffer, actually goes down -- rather
 * than just "we wrote some registers and nothing crashed".
 */
#include "ac97.h"
#include "pci.h"
#include "io.h"
#include "pmm.h"
#include "kstring.h"
#include "debugcon.h"

/* ---- BAR0: native audio mixer (the codec) --------------------------- */
#define NAM_RESET        0x00   /* any write resets the codec */
#define NAM_MASTER_VOL   0x02   /* 0 = loudest, 0x8000 = mute */
#define NAM_PCM_OUT_VOL  0x18
#define NAM_EXT_AUDIO_ID 0x28   /* bit0: variable rate audio supported */
#define NAM_EXT_AUDIO_CTL 0x2A  /* bit0: enable variable rate audio */
#define NAM_PCM_DAC_RATE 0x2C   /* sample rate in Hz, once VRA is on */

/* ---- BAR1: native audio bus master (the DMA engine) ------------------ */
/* The PCM *out* channel's register box starts at 0x10; there are identical
 * boxes at 0x00 (PCM in) and 0x20 (mic in) that we do not use. */
#define NABM_PO_BDBAR    0x10   /* 32-bit physical base of the descriptor list */
#define NABM_PO_CIV      0x14   /* 8-bit  current index */
#define NABM_PO_LVI      0x15   /* 8-bit  last valid index */
#define NABM_PO_SR       0x16   /* 16-bit status */
#define NABM_PO_PICB     0x18   /* 16-bit samples left in the current buffer */
#define NABM_PO_CR       0x1B   /* 8-bit  control */
#define NABM_GLOB_CNT    0x2C   /* 32-bit global control */

#define PO_CR_RPBM       0x01   /* run/pause bus master: 1 = play */
#define PO_CR_RR         0x02   /* reset this channel's registers */
#define PO_SR_DCH        0x01   /* DMA controller halted */

#define GLOB_CNT_COLD    0x02   /* 0 = hold cold reset, 1 = release */

/* 16-bit signed stereo at 48 kHz is the one format every AC'97 codec has to
 * support, so it needs no negotiation. */
#define SAMPLE_RATE  48000
#define CHANNELS     2
#define TONE_HZ      440

/* One page per buffer: 4096 bytes = 2048 samples = 1024 stereo frames,
 * about 21 ms of audio each.  Two of them is a tone long enough to hear and
 * short enough that the self-test does not stall the boot. */
#define NBUF     2
#define BUF_BYTES   4096
#define BUF_SAMPLES (BUF_BYTES / 2)          /* the BDL counts 16-bit samples */

typedef struct __attribute__((packed)) {
    uint32_t addr;        /* physical address of the audio data */
    uint16_t samples;     /* number of 16-bit samples, NOT bytes */
    uint16_t flags;       /* bit15 = interrupt on completion, bit14 = BUP */
} ac97_bd_t;

static uint16_t g_nam, g_nabm;     /* I/O port bases of the two banks */
static int      g_ok;
static ac97_bd_t *g_bdl;  static uint64_t g_bdl_phys;
static int16_t   *g_buf[NBUF];  static uint64_t g_buf_phys[NBUF];

static void spin(unsigned n)
{
    while (n--)
        io_delay();
}

int ac97_present(void) { return g_ok; }

/* A square wave is the honest choice here: it is exactly representable in
 * integers, so a wrong sample rate or a swapped channel is audible and
 * obvious rather than subtly off. */
static void fill_tone(int16_t *dst, uint32_t frames, uint32_t *phase)
{
    const uint32_t period = SAMPLE_RATE / TONE_HZ;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = (*phase % period) < period / 2 ? 8000 : -8000;
        dst[i * CHANNELS + 0] = v;
        dst[i * CHANNELS + 1] = v;
        (*phase)++;
    }
}

int ac97_init(void)
{
    const pci_dev_t *d = pci_find(PCI_VENDOR_INTEL, AC97_DEV_82801AA);
    if (!d) {
        dbg_puts("AC97: no 8086:2415 on the bus, skipping\r\n");
        return 0;
    }

    pci_enable(d);                       /* I/O space + bus master */
    g_nam  = pci_bar_io(d, 0);
    g_nabm = pci_bar_io(d, 1);
    if (!g_nam || !g_nabm) {
        dbg_puts("AC97: BARs are not in I/O space, skipping\r\n");
        return 0;
    }

    /* Release the cold reset and let the codec come up, then reset the
     * mixer itself: after this every mixer register is at its default. */
    outl(g_nabm + NABM_GLOB_CNT, GLOB_CNT_COLD);
    spin(1000);
    outw(g_nam + NAM_RESET, 0);
    spin(1000);

    /* Volume registers default to *muted* on a real codec, which is the
     * classic "my driver works but there is no sound" bug.  0 is full
     * volume (attenuation 0 dB). */
    outw(g_nam + NAM_MASTER_VOL, 0x0000);
    outw(g_nam + NAM_PCM_OUT_VOL, 0x0000);

    /* Ask for 48 kHz.  Codecs that support variable rate audio need VRA
     * turned on before the rate register does anything; ones that do not
     * are fixed at 48 kHz already, which is what we wanted anyway. */
    uint16_t ext = inw(g_nam + NAM_EXT_AUDIO_ID);
    if (ext & 0x1) {
        outw(g_nam + NAM_EXT_AUDIO_CTL,
             (uint16_t)(inw(g_nam + NAM_EXT_AUDIO_CTL) | 0x1));
        outw(g_nam + NAM_PCM_DAC_RATE, SAMPLE_RATE);
    }

    /* Reset the PCM-out channel's own registers (CIV, LVI, PICB, SR). */
    outb(g_nabm + NABM_PO_CR, PO_CR_RR);
    for (int i = 0; i < 1000 && (inb(g_nabm + NABM_PO_CR) & PO_CR_RR); i++)
        io_delay();

    /* DMA memory: the descriptor list and the audio buffers. */
    g_bdl_phys = pmm_alloc_zeroed();
    if (!g_bdl_phys) {
        dbg_puts("AC97: out of memory for the descriptor list\r\n");
        return 0;
    }
    g_bdl = (ac97_bd_t *)pmm_virt(g_bdl_phys);
    for (int i = 0; i < NBUF; i++) {
        g_buf_phys[i] = pmm_alloc_zeroed();
        if (!g_buf_phys[i]) {
            dbg_puts("AC97: out of memory for the audio buffers\r\n");
            return 0;
        }
        g_buf[i] = (int16_t *)pmm_virt(g_buf_phys[i]);
    }

    g_ok = 1;
    dbg_puts("AC97: 8086:2415 up, nam=0x");
    dbg_puts_hexn(g_nam, 4);
    dbg_puts(" nabm=0x");
    dbg_puts_hexn(g_nabm, 4);
    dbg_puts(" rate=");
    dbg_puts_dec(SAMPLE_RATE);
    dbg_puts("Hz\r\n");
    return 1;
}

int ac97_selftest(void)
{
    if (!g_ok)
        return 0;

    /* Fill both buffers with one continuous tone -- the phase carries over
     * so there is no click at the buffer boundary. */
    uint32_t phase = 0;
    for (int i = 0; i < NBUF; i++) {
        fill_tone(g_buf[i], BUF_SAMPLES / CHANNELS, &phase);
        g_bdl[i].addr    = (uint32_t)g_buf_phys[i];
        g_bdl[i].samples = BUF_SAMPLES;
        g_bdl[i].flags   = 0;
    }

    /* Point the engine at the list, mark the last entry it may play, go. */
    outl(g_nabm + NABM_PO_BDBAR, (uint32_t)g_bdl_phys);
    outb(g_nabm + NABM_PO_LVI, NBUF - 1);
    outb(g_nabm + NABM_PO_CR, PO_CR_RPBM);

    /* Loading the first descriptor clears "DMA controller halted" and sets
     * PICB to that buffer's sample count.  If this never happens the engine
     * did not accept the list -- almost always a missing bus-master enable
     * or a BDL address the card cannot reach. */
    uint16_t picb0 = 0;
    int armed = 0;
    for (int i = 0; i < 100000; i++) {
        uint16_t sr = inw(g_nabm + NABM_PO_SR);
        picb0 = inw(g_nabm + NABM_PO_PICB);
        if (!(sr & PO_SR_DCH) && picb0 != 0) {
            armed = 1;
            break;
        }
        io_delay();
    }
    if (!armed) {
        dbg_puts("AC97: self-test FAIL (DMA engine never started)\r\n");
        outb(g_nabm + NABM_PO_CR, 0);
        return 0;
    }

    /* Now the falsifiable part: the play position has to move.  QEMU drives
     * the codec off host wall-clock time, so busy-waiting here really does
     * let samples drain. */
    int consumed = 0;
    for (int i = 0; i < 400000; i++) {
        uint16_t picb = inw(g_nabm + NABM_PO_PICB);
        uint8_t  civ  = inb(g_nabm + NABM_PO_CIV);
        if (civ != 0 || picb < picb0) {
            consumed = 1;
            break;
        }
        io_delay();
    }

    outb(g_nabm + NABM_PO_CR, 0);        /* stop; do not tie up the codec */

    if (!consumed) {
        dbg_puts("AC97: self-test FAIL (armed but no samples were played)\r\n");
        return 0;
    }

    dbg_puts("AC97: playback self-test PASS, ");
    dbg_puts_dec(TONE_HZ);
    dbg_puts("Hz tone streamed over ");
    dbg_puts_dec(NBUF);
    dbg_puts(" DMA buffer(s)\r\n");
    return 1;
}
