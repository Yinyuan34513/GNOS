/*
 * hda.c — Intel High Definition Audio controller + codec driver. (GPLv2)
 *
 * See hda.h for why this device needs twice as much code as AC'97.  The
 * short version: the controller is a fixed register block, the codec is a
 * self-describing tree, and nothing works until the two agree on a number.
 *
 * That number is the *stream tag*.  A controller stream descriptor is a DMA
 * engine with no opinion about audio; a codec converter is a DAC with no
 * opinion about memory.  They are joined by putting the same 4-bit tag in
 * SDnCTL[23:20] and in the converter's SET_CHANNEL_STREAMID payload.  Get
 * them out of step and everything reports success while the link carries
 * nothing -- the classic silent HDA bug, and the reason this driver checks
 * the DMA position register rather than trusting its own writes.
 *
 * The other half of the agreement is the sample format, which has to be
 * written twice: once to SDnFMT so the controller knows how fast to drain
 * the ring, and once to the converter so it knows how to interpret what
 * arrives.  Same 16-bit encoding both times.
 */
#include "hda.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "pmm.h"
#include "kstring.h"
#include "timer.h"
#include "debugcon.h"

/* ---- controller registers (from BAR0) -------------------------------- */
#define HDA_GCAP      0x00      /* 16-bit: how many streams of each kind */
#define HDA_VMAJ      0x03      /* 8-bit  spec version */
#define HDA_VMIN      0x02
#define HDA_GCTL      0x08      /* 32-bit global control */
#define HDA_STATESTS  0x0E      /* 16-bit: bit N set = codec answered at N */
#define HDA_ICW       0x60      /* 32-bit immediate command word */
#define HDA_IRR       0x64      /* 32-bit immediate response */
#define HDA_ICS       0x68      /* 16-bit immediate command status */

#define GCTL_CRST     (1u << 0) /* 0 = hold controller reset, 1 = release */
#define ICS_BUSY      (1u << 0) /* we set it; the controller clears it */
#define ICS_VALID     (1u << 1) /* a response is sitting in IRR (write 1 = ack) */

/* GCAP tells us how the stream descriptors are laid out.  Input streams come
 * first, then bidirectional ones, then output; each descriptor is a 0x20-byte
 * block starting at 0x80.  So the first *output* stream is at index ISS+BSS,
 * which is 4 on everything QEMU emulates but is not a constant in general. */
#define GCAP_ISS(g)   (((g) >> 8) & 0x0F)
#define GCAP_BSS(g)   (((g) >> 3) & 0x1F)
#define GCAP_OSS(g)   (((g) >> 12) & 0x0F)

/* ---- stream descriptor registers (from SD_BASE(n)) ------------------- */
#define SD_BASE(n)    (0x80u + (n) * 0x20u)
#define SD_CTL        0x00      /* 24-bit control; byte 0 holds RESET/RUN */
#define SD_STS        0x03      /* 8-bit status */
#define SD_LPIB       0x04      /* 32-bit link position in buffer */
#define SD_CBL        0x08      /* 32-bit cyclic buffer length, in bytes */
#define SD_LVI        0x0C      /* 16-bit last valid BDL index */
#define SD_FMT        0x12      /* 16-bit sample format */
#define SD_BDLPL      0x18      /* 32-bit BDL base, low  (128-byte aligned) */
#define SD_BDLPU      0x1C      /* 32-bit BDL base, high */

#define SDCTL_SRST    (1u << 0) /* stream reset: set, wait, clear, wait */
#define SDCTL_RUN     (1u << 1) /* DMA go */
#define SDCTL_TAG_SHIFT 20

/* ---- codec verbs ------------------------------------------------------ */
/* A command word is  cad<<28 | nid<<20 | verb<<8 | payload.  Verbs whose id
 * is 12 bits take an 8-bit payload; verbs whose id is 4 bits (0x2xx, 0x3xx)
 * take a 16-bit one.  Shifting the id left by 8 lands both in the right
 * place, which is why one macro covers all of them. */
#define VERB_GET_PARAMETER        0xF00
#define VERB_GET_CONNECT_LIST     0xF02
#define VERB_SET_STREAM_FORMAT    0x200
#define VERB_SET_AMP_GAIN_MUTE    0x300
#define VERB_SET_POWER_STATE      0x705
#define VERB_SET_CHANNEL_STREAMID 0x706
#define VERB_SET_PIN_WIDGET_CTL   0x707

/* GET_PARAMETER parameter ids */
#define PAR_NODE_COUNT     0x04   /* [23:16] first sub-node, [7:0] how many */
#define PAR_FUNCTION_TYPE  0x05
#define PAR_WIDGET_CAP     0x09
#define PAR_PIN_CAP        0x0C
#define PAR_CONNLIST_LEN   0x0E

#define FUNC_TYPE_AUDIO    0x01

#define WCAP_CONN_LIST     (1u << 8)
#define WCAP_OUT_AMP       (1u << 2)
#define WCAP_TYPE(c)       (((c) >> 20) & 0x0F)
#define WID_AUD_OUT        0x0    /* a DAC */
#define WID_PIN            0x4    /* a physical jack */

#define PINCAP_OUT         (1u << 4)
#define PINCTL_OUT_EN      (1u << 6)

/* SET_AMP_GAIN_MUTE payload: which amp, which channel, mute bit, gain. */
#define AMP_SET_OUTPUT     (1u << 15)
#define AMP_SET_LEFT       (1u << 13)
#define AMP_SET_RIGHT      (1u << 12)
#define AMP_MUTE           (1u << 7)

#define PWRST_D0           0x00

/*
 * Sample format, shared by SDnFMT and SET_STREAM_FORMAT:
 *   bit14   base rate: 0 = 48 kHz, 1 = 44.1 kHz
 *   [13:11] multiplier - 1     [10:8] divisor - 1
 *   [6:4]   bits per sample: 001 = 16
 *   [3:0]   channels - 1
 * 0x0011 is therefore plain 48 kHz 16-bit stereo, the format every codec
 * supports and the one QEMU's mixer wants anyway.
 */
#define FMT_48K_S16_STEREO 0x0011

#define SAMPLE_RATE 48000
#define CHANNELS    2
#define TONE_HZ     440
#define STREAM_TAG  1          /* any non-zero 4-bit value will do */

/* Two pages of audio, played as a cycle: 8192 bytes / 4 bytes per frame =
 * 2048 frames, about 43 ms per lap.  The self-test lets it lap a few times
 * and then stops it. */
#define NBUF        2
#define BUF_BYTES   4096
#define TOTAL_BYTES (NBUF * BUF_BYTES)

/* A buffer descriptor.  Unlike AC'97's, the length is in *bytes* and the
 * address is a full 64 bits, so there is no 4 GiB ceiling on where the
 * audio may live. */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;     /* bit0 = interrupt on completion */
} hda_bdl_t;

static volatile uint8_t *g_regs;
static int      g_ok;
static uint32_t g_codec;        /* codec address that answered STATESTS */
static uint32_t g_dac, g_pin;   /* the converter and the jack it feeds */
static uint32_t g_ostream;      /* index of the output stream descriptor */

static hda_bdl_t *g_bdl;   static uint64_t g_bdl_phys;
static int16_t   *g_buf[NBUF];  static uint64_t g_buf_phys[NBUF];

static inline uint8_t  r8 (uint32_t o) { return *(volatile uint8_t  *)(g_regs + o); }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(g_regs + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(g_regs + o); }
static inline void w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g_regs + o) = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_regs + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_regs + o) = v; }

int hda_present(void) { return g_ok; }

/* ---- the immediate command interface --------------------------------- */
/*
 * Write the verb to ICW, set BUSY in ICS, and wait for the controller to
 * clear BUSY and raise VALID.  VALID is write-1-to-clear, so the stale
 * result of the previous command has to be acknowledged first -- otherwise
 * the very first poll succeeds against an answer to a different question.
 */
static int codec_cmd(uint32_t nid, uint32_t verb, uint32_t payload,
                     uint32_t *resp)
{
    int i;

    for (i = 0; i < 10000; i++) {
        if (!(r16(HDA_ICS) & ICS_BUSY))
            break;
        io_delay();
    }
    if (r16(HDA_ICS) & ICS_BUSY)
        return 0;

    w16(HDA_ICS, ICS_VALID);            /* ack whatever was left over */
    w32(HDA_ICW, (g_codec << 28) | (nid << 20) | (verb << 8) | payload);
    w16(HDA_ICS, ICS_BUSY);

    for (i = 0; i < 10000; i++) {
        uint16_t s = r16(HDA_ICS);
        if (!(s & ICS_BUSY) && (s & ICS_VALID)) {
            uint32_t v = r32(HDA_IRR);
            if (resp)
                *resp = v;
            return 1;
        }
        io_delay();
    }
    return 0;
}

static uint32_t codec_param(uint32_t nid, uint32_t par)
{
    uint32_t v = 0;
    codec_cmd(nid, VERB_GET_PARAMETER, par, &v);
    return v;
}

/* ---- walking the codec ----------------------------------------------- */
/*
 * The root node names a range of function groups; the audio function group
 * names a range of widgets; each widget's capability word says what it is.
 * We want the first DAC and an output-capable pin that can reach it.
 *
 * "Can reach it" is worth checking rather than assuming: a codec with an
 * HDMI converter and an analogue one will happily let you tag the wrong
 * pair, and the failure is silence, not an error.
 */
static int pin_reaches(uint32_t pin, uint32_t wcap, uint32_t dac)
{
    if (!(wcap & WCAP_CONN_LIST))
        return 1;               /* no list published: nothing to contradict */

    uint32_t len = codec_param(pin, PAR_CONNLIST_LEN) & 0x7F;
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t list = 0;
        if (!codec_cmd(pin, VERB_GET_CONNECT_LIST, i, &list))
            return 0;
        for (int b = 0; b < 4 && i + (uint32_t)b < len; b++)
            if (((list >> (b * 8)) & 0xFF) == dac)
                return 1;
    }
    return 0;
}

static int find_output_path(void)
{
    uint32_t nc = codec_param(0, PAR_NODE_COUNT);
    uint32_t fg_first = (nc >> 16) & 0xFF, fg_count = nc & 0xFF;

    for (uint32_t f = 0; f < fg_count; f++) {
        uint32_t fg = fg_first + f;
        if ((codec_param(fg, PAR_FUNCTION_TYPE) & 0x7F) != FUNC_TYPE_AUDIO)
            continue;

        nc = codec_param(fg, PAR_NODE_COUNT);
        uint32_t w_first = (nc >> 16) & 0xFF, w_count = nc & 0xFF;

        uint32_t dac = 0;
        for (uint32_t i = 0; i < w_count && !dac; i++) {
            uint32_t nid = w_first + i;
            if (WCAP_TYPE(codec_param(nid, PAR_WIDGET_CAP)) == WID_AUD_OUT)
                dac = nid;
        }
        if (!dac)
            continue;

        for (uint32_t i = 0; i < w_count; i++) {
            uint32_t nid = w_first + i;
            uint32_t wcap = codec_param(nid, PAR_WIDGET_CAP);
            if (WCAP_TYPE(wcap) != WID_PIN)
                continue;
            if (!(codec_param(nid, PAR_PIN_CAP) & PINCAP_OUT))
                continue;
            if (!pin_reaches(nid, wcap, dac))
                continue;
            g_dac = dac;
            g_pin = nid;
            return 1;
        }
    }
    return 0;
}

/* Power the path up, agree on a format, join it to our stream tag, and open
 * the output.  Amplifiers come out of reset muted, same as AC'97. */
static void configure_path(void)
{
    codec_cmd(g_dac, VERB_SET_POWER_STATE, PWRST_D0, NULL);
    codec_cmd(g_pin, VERB_SET_POWER_STATE, PWRST_D0, NULL);

    codec_cmd(g_dac, VERB_SET_STREAM_FORMAT, FMT_48K_S16_STEREO, NULL);
    codec_cmd(g_dac, VERB_SET_CHANNEL_STREAMID, (STREAM_TAG << 4) | 0, NULL);

    codec_cmd(g_dac, VERB_SET_AMP_GAIN_MUTE,
              AMP_SET_OUTPUT | AMP_SET_LEFT | AMP_SET_RIGHT | 0x2A, NULL);
    codec_cmd(g_pin, VERB_SET_PIN_WIDGET_CTL, PINCTL_OUT_EN, NULL);
}

/* ---- bring-up --------------------------------------------------------- */
int hda_init(void)
{
    const pci_dev_t *d = pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA);
    if (!d) {
        dbg_puts("HDA: no class 04:03 controller on the bus, skipping\r\n");
        return 0;
    }

    pci_enable(d);                       /* memory space + BUS MASTER */
    uint64_t base = pci_map_bar(d, 0);
    if (!base) {
        dbg_puts("HDA: BAR0 is not a memory region, skipping\r\n");
        return 0;
    }
    g_regs = (volatile uint8_t *)base;

    /* Reset: drop CRST, wait for the controller to admit it is down, then
     * raise it again.  Skipping the "wait for 0" half is the usual reason a
     * driver reads garbage out of STATESTS a moment later. */
    w32(HDA_GCTL, r32(HDA_GCTL) & ~GCTL_CRST);
    for (int i = 0; i < 10000 && (r32(HDA_GCTL) & GCTL_CRST); i++)
        io_delay();
    w32(HDA_GCTL, GCTL_CRST);
    for (int i = 0; i < 10000 && !(r32(HDA_GCTL) & GCTL_CRST); i++)
        io_delay();
    if (!(r32(HDA_GCTL) & GCTL_CRST)) {
        dbg_puts("HDA: controller never came out of reset\r\n");
        return 0;
    }

    /* Codecs need ~521 us after reset to finish announcing themselves; the
     * spec makes this the driver's problem, not the controller's. */
    timer_delay_ms(1);

    uint16_t sts = r16(HDA_STATESTS);
    if (!sts) {
        dbg_puts("HDA: controller is up but no codec answered\r\n");
        return 0;
    }
    for (g_codec = 0; g_codec < 15; g_codec++)
        if (sts & (1u << g_codec))
            break;

    uint16_t gcap = r16(HDA_GCAP);
    if (!GCAP_OSS(gcap)) {
        dbg_puts("HDA: controller has no output streams\r\n");
        return 0;
    }
    g_ostream = GCAP_ISS(gcap) + GCAP_BSS(gcap);

    if (!find_output_path()) {
        dbg_puts("HDA: codec has no DAC-to-output-pin path\r\n");
        return 0;
    }
    configure_path();

    /* DMA memory: the descriptor list (which the spec requires to be at
     * least 128-byte aligned -- a page is more than enough) and the audio. */
    g_bdl_phys = pmm_alloc_zeroed();
    if (!g_bdl_phys) {
        dbg_puts("HDA: out of memory for the descriptor list\r\n");
        return 0;
    }
    g_bdl = (hda_bdl_t *)pmm_virt(g_bdl_phys);
    for (int i = 0; i < NBUF; i++) {
        g_buf_phys[i] = pmm_alloc_zeroed();
        if (!g_buf_phys[i]) {
            dbg_puts("HDA: out of memory for the audio buffers\r\n");
            return 0;
        }
        g_buf[i] = (int16_t *)pmm_virt(g_buf_phys[i]);
    }

    g_ok = 1;
    dbg_puts("HDA: ");
    dbg_puts_hexn(d->vendor, 4);
    dbg_putc(':');
    dbg_puts_hexn(d->device, 4);
    dbg_puts(" up, version ");
    dbg_puts_dec(r8(HDA_VMAJ));
    dbg_putc('.');
    dbg_puts_dec(r8(HDA_VMIN));
    dbg_puts(", codec ");
    dbg_puts_dec(g_codec);
    dbg_puts(": dac nid ");
    dbg_puts_dec(g_dac);
    dbg_puts(" -> pin nid ");
    dbg_puts_dec(g_pin);
    dbg_puts(", output stream ");
    dbg_puts_dec(g_ostream);
    dbg_puts("\r\n");
    return 1;
}

/* ---- self-test -------------------------------------------------------- */
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

int hda_selftest(void)
{
    if (!g_ok)
        return 0;

    const uint32_t sd = SD_BASE(g_ostream);
    uint32_t phase = 0;

    for (int i = 0; i < NBUF; i++) {
        fill_tone(g_buf[i], BUF_BYTES / (2 * CHANNELS), &phase);
        g_bdl[i].addr  = g_buf_phys[i];
        g_bdl[i].len   = BUF_BYTES;
        g_bdl[i].flags = 0;              /* no interrupt: we are polling */
    }

    /* Stream reset is a handshake, not a poke: the bit reads back as 1 while
     * the engine is held down and as 0 once it has let go.  Programming the
     * descriptor in between the two states is how you get a stream that
     * starts with the previous run's buffer address. */
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~SDCTL_RUN));
    w8(sd + SD_CTL, SDCTL_SRST);
    for (int i = 0; i < 10000 && !(r8(sd + SD_CTL) & SDCTL_SRST); i++)
        io_delay();
    w8(sd + SD_CTL, 0);
    for (int i = 0; i < 10000 && (r8(sd + SD_CTL) & SDCTL_SRST); i++)
        io_delay();

    w32(sd + SD_BDLPL, (uint32_t)g_bdl_phys);
    w32(sd + SD_BDLPU, (uint32_t)(g_bdl_phys >> 32));
    w32(sd + SD_CBL, TOTAL_BYTES);
    w16(sd + SD_LVI, NBUF - 1);
    w16(sd + SD_FMT, FMT_48K_S16_STEREO);
    w32(sd + SD_CTL, (uint32_t)STREAM_TAG << SDCTL_TAG_SHIFT);

    uint32_t lpib0 = r32(sd + SD_LPIB);
    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) | SDCTL_RUN));

    /* The falsifiable part.  LPIB is written by the controller as it drains
     * the ring, so if it moves, the tag matched, the BDL parsed, and bus
     * mastering is really on.  Nothing else makes it move. */
    uint32_t lpib = lpib0;
    int moved = 0;
    for (int ms = 0; ms < 300; ms++) {
        timer_delay_ms(1);
        lpib = r32(sd + SD_LPIB);
        if (lpib != lpib0) {
            moved = 1;
            break;
        }
    }

    w8(sd + SD_CTL, (uint8_t)(r8(sd + SD_CTL) & ~SDCTL_RUN));
    codec_cmd(g_dac, VERB_SET_CHANNEL_STREAMID, 0, NULL);

    if (!moved) {
        dbg_puts("HDA: playback self-test FAIL (stream armed but LPIB never moved)\r\n");
        return 0;
    }

    dbg_puts("HDA: playback self-test PASS, ");
    dbg_puts_dec(TONE_HZ);
    dbg_puts("Hz tone streamed on tag ");
    dbg_puts_dec(STREAM_TAG);
    dbg_puts(", LPIB advanced ");
    dbg_puts_dec(lpib - lpib0);
    dbg_puts(" bytes of ");
    dbg_puts_dec(TOTAL_BYTES);
    dbg_puts("\r\n");
    return 1;
}
