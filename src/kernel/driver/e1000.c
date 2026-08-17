/*
 * e1000.c — Intel 82540EM gigabit ethernet driver. (GPLv2)
 *
 * Bring-up order, and why each step exists:
 *
 *   1. Find the card on the PCI bus and turn on *bus mastering*.  Without
 *      that bit the card is not allowed to issue DMA reads, so it can never
 *      fetch a descriptor and every transmit silently disappears.  This is
 *      the single most common way to get a "working" e1000 driver that sends
 *      nothing at all.
 *   2. Map BAR0 uncacheable.  Device registers are not memory: a cached read
 *      would happily return the value from five microseconds ago.
 *   3. Reset, then read the MAC out of the EEPROM.
 *   4. Allocate the descriptor rings and their buffers from the physical page
 *      allocator, because the card sees *physical* addresses -- it has no
 *      idea our page tables exist.
 *   5. Point RDBAL/RDLEN/RDH/RDT and TDBAL/TDLEN/TDH/TDT at those rings and
 *      set the enable bits in RCTL/TCTL.
 *   6. Hook the card's PCI interrupt line and unmask the causes we care
 *      about, so that at run time the kernel is told about arriving frames
 *      instead of having to poll for them.
 *
 * After that, sending is "fill descriptor, bump the tail register, wait for
 * the card to set Descriptor Done", and receiving is the mirror image.
 */
#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "idt.h"
#include "vmm.h"
#include "pmm.h"
#include "kstring.h"
#include "timer.h"
#include "debugcon.h"

/* ---- register offsets (from BAR0) ---------------------------------- */
#define E1000_CTRL    0x0000    /* device control */
#define E1000_STATUS  0x0008    /* device status */
#define E1000_EERD    0x0014    /* EEPROM read */
#define E1000_MDIC    0x0020    /* PHY management interface */
#define E1000_ICR     0x00C0    /* interrupt cause read (read = ack) */
#define E1000_IMS     0x00D0    /* interrupt mask set */
#define E1000_IMC     0x00D8    /* interrupt mask clear */
#define E1000_RCTL    0x0100    /* receive control */
#define E1000_TCTL    0x0400    /* transmit control */
#define E1000_TIPG    0x0410    /* transmit inter-packet gap */
#define E1000_RDBAL   0x2800    /* rx ring base, low 32 bits */
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808    /* rx ring length in BYTES */
#define E1000_RDH     0x2810    /* rx head  (card writes) */
#define E1000_RDT     0x2818    /* rx tail  (we write) */
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_MTA     0x5200    /* multicast table array, 128 dwords */
#define E1000_RAL0    0x5400    /* receive address, low  */
#define E1000_RAH0    0x5404    /* receive address, high (bit31 = valid) */

/* Statistics.  These are the only way to tell "the MAC never saw the frame"
 * apart from "the MAC saw it and could not put it anywhere", which are two
 * completely different bugs that look identical from the descriptor ring. */
#define E1000_CRCERRS 0x4000    /* CRC errors */
#define E1000_MPC     0x4010    /* missed packets (no buffer available) */
#define E1000_GPRC    0x4074    /* good packets received */
#define E1000_RNBC    0x40A0    /* receive no buffers */
#define E1000_TPR     0x40D0    /* total packets received (before filter) */
#define E1000_TPT     0x40D4    /* total packets transmitted */

/* CTRL bits */
#define CTRL_SLU      (1u << 6)    /* set link up */
#define CTRL_RST      (1u << 26)   /* device reset (self-clearing) */

/* STATUS bits */
#define STATUS_LU     (1u << 1)    /* link up */
#define STATUS_FD     (1u << 0)    /* full duplex */

/* Interrupt causes (same bit layout in ICR, IMS and IMC) */
#define ICR_TXDW      (1u << 0)    /* transmit descriptor written back */
#define ICR_LSC       (1u << 2)    /* link status change */
#define ICR_RXDMT0    (1u << 4)    /* rx ring is running low */
#define ICR_RXO       (1u << 6)    /* receiver overrun */
#define ICR_RXT0      (1u << 7)    /* receive timer: frames are waiting */

/* RCTL bits */
#define RCTL_EN       (1u << 1)    /* receiver enable */
#define RCTL_UPE      (1u << 3)    /* unicast promiscuous */
#define RCTL_MPE      (1u << 4)    /* multicast promiscuous */
#define RCTL_BAM      (1u << 15)   /* accept broadcast */
#define RCTL_SECRC    (1u << 26)   /* strip the ethernet CRC for us */
/* BSIZE == 00 with no BSEX means 2048-byte buffers, which is what we
 * allocate, so there is no bit to set for it. */

/* TCTL bits */
#define TCTL_EN       (1u << 1)    /* transmitter enable */
#define TCTL_PSP      (1u << 3)    /* pad short packets to 64 bytes */
#define TCTL_CT_SHIFT   4          /* collision threshold */
#define TCTL_COLD_SHIFT 12         /* collision distance */

/* transmit descriptor command bits */
#define TXD_CMD_EOP   0x01         /* end of packet */
#define TXD_CMD_IFCS  0x02         /* insert the FCS/CRC for us */
#define TXD_CMD_RS    0x08         /* report status when done */
#define TXD_STAT_DD   0x01         /* descriptor done */

/* receive descriptor status bits */
#define RXD_STAT_DD   0x01
#define RXD_STAT_EOP  0x02

/* PHY registers, reached through MDIC.  The e1000's PHY is always at
 * address 1; register 0 is the MII basic control register, whose bit 14 is
 * "loopback": frames handed to the transmitter come straight back up the
 * receive path without ever touching a wire.  That is what lets this driver
 * prove itself on a headless machine with no network at all. */
#define PHY_ADDR      1
#define PHY_BMCR      0
#define BMCR_LOOPBACK 0x4000

#define NRX 16                 /* rx descriptors (ring must be 128B-aligned) */
#define NTX 8                  /* tx descriptors */
#define BUFSZ 2048             /* per-descriptor buffer, matches RCTL BSIZE */

/* Legacy descriptor layouts, exactly as the card expects them in memory.
 * packed matters: a stray compiler pad byte here and the card reads garbage. */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} tx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_desc_t;

static volatile uint8_t *g_regs;        /* BAR0, uncacheable */
static int      g_ok;
static uint8_t  g_mac[6];
static uint8_t  g_irq;

/* volatile: these words are written by the card behind the CPU's back, so
 * every read has to actually happen. */
static volatile tx_desc_t *g_tx;  static uint64_t g_tx_phys;
static volatile rx_desc_t *g_rx;  static uint64_t g_rx_phys;
static uint8_t   *g_txbuf[NTX];  static uint64_t g_txbuf_phys[NTX];
static uint8_t   *g_rxbuf[NRX];  static uint64_t g_rxbuf_phys[NRX];
static unsigned   g_tx_tail;
static unsigned   g_rx_head;

static uint64_t g_tx_frames, g_rx_frames, g_irqs;

static inline uint32_t reg_read(uint32_t off)
{
    return *(volatile uint32_t *)(g_regs + off);
}

static inline void reg_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(g_regs + off) = val;
}

/* Interrupts are not on yet when this driver initialises, so "waiting" can
 * only be a spin.  io_delay() gives each iteration a predictable, tiny cost
 * and a compiler barrier, which spin loops over DMA memory need. */
static void spin(unsigned n)
{
    while (n--)
        io_delay();
}

/* ---- EEPROM ---------------------------------------------------------- */
/* The MAC address is burned into a serial EEPROM the card reads on our
 * behalf: write the word address with the START bit, poll for DONE, and the
 * 16-bit result appears in the top half of the same register. */
static int eeprom_read(uint8_t word, uint16_t *out)
{
    reg_write(E1000_EERD, ((uint32_t)word << 8) | 1u);
    for (int i = 0; i < 10000; i++) {
        uint32_t v = reg_read(E1000_EERD);
        if (v & (1u << 4)) {
            *out = (uint16_t)(v >> 16);
            return 1;
        }
        io_delay();
    }
    return 0;
}

/* ---- PHY ------------------------------------------------------------- */
static void phy_write(uint8_t reg, uint16_t val)
{
    reg_write(E1000_MDIC, (uint32_t)val |
                          ((uint32_t)reg << 16) |
                          ((uint32_t)PHY_ADDR << 21) |
                          (1u << 26));            /* OP = write */
    for (int i = 0; i < 10000; i++) {
        if (reg_read(E1000_MDIC) & (1u << 28))    /* R: ready */
            return;
        io_delay();
    }
}

static uint16_t phy_read(uint8_t reg)
{
    reg_write(E1000_MDIC, ((uint32_t)reg << 16) |
                          ((uint32_t)PHY_ADDR << 21) |
                          (2u << 26));            /* OP = read */
    for (int i = 0; i < 10000; i++) {
        uint32_t v = reg_read(E1000_MDIC);
        if (v & (1u << 28))
            return (uint16_t)v;
        io_delay();
    }
    return 0xFFFF;
}

/* ---- interrupts ------------------------------------------------------ */
/*
 * Reading ICR is what acknowledges the interrupt at the card; if the handler
 * forgets, the line stays asserted and the machine livelocks in the IRQ
 * stub.  There is no work queue above this driver yet, so all the handler
 * does is ack, count, and note a link transition -- arriving frames sit in
 * the ring until someone calls e1000_recv().
 */
static void e1000_irq(regs_t *r)
{
    (void)r;
    uint32_t icr = reg_read(E1000_ICR);
    if (!icr)
        return;                          /* not us: PCI lines are shared */

    g_irqs++;

    if (icr & ICR_LSC) {
        dbg_puts("E1000: link ");
        dbg_puts((reg_read(E1000_STATUS) & STATUS_LU) ? "up\r\n" : "down\r\n");
    }
    if (icr & ICR_RXO)
        dbg_puts("E1000: receiver overrun (ring not drained)\r\n");
}

/* ---- ring setup ------------------------------------------------------ */
/* One physical frame per buffer is wasteful (we use 2 KiB of each 4 KiB
 * page) but it keeps every DMA address page-aligned, which removes a whole
 * class of "the buffer straddles a page and the card doesn't know" bug that
 * has nothing to teach. */
static int alloc_buffers(void)
{
    for (int i = 0; i < NTX; i++) {
        g_txbuf_phys[i] = pmm_alloc_zeroed();
        if (!g_txbuf_phys[i])
            return 0;
        g_txbuf[i] = (uint8_t *)pmm_virt(g_txbuf_phys[i]);
    }
    for (int i = 0; i < NRX; i++) {
        g_rxbuf_phys[i] = pmm_alloc_zeroed();
        if (!g_rxbuf_phys[i])
            return 0;
        g_rxbuf[i] = (uint8_t *)pmm_virt(g_rxbuf_phys[i]);
    }
    return 1;
}

static void rx_init(void)
{
    for (int i = 0; i < NRX; i++) {
        g_rx[i].addr   = g_rxbuf_phys[i];
        g_rx[i].status = 0;
    }
    reg_write(E1000_RDBAL, (uint32_t)(g_rx_phys & 0xFFFFFFFF));
    reg_write(E1000_RDBAH, (uint32_t)(g_rx_phys >> 32));
    reg_write(E1000_RDLEN, NRX * (uint32_t)sizeof(rx_desc_t));
    reg_write(E1000_RDH, 0);
    /* The tail points one past the last descriptor the card may write, so
     * handing it NRX-1 gives it the whole ring minus one slot. */
    reg_write(E1000_RDT, NRX - 1);
    g_rx_head = 0;

    /* Promiscuous: in loopback the frame we get back was addressed to us
     * anyway, but promiscuous mode means a misconfigured RAL/RAH cannot
     * quietly turn a working test into a failing one. */
    reg_write(E1000_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM |
                          RCTL_SECRC);
}

static void tx_init(void)
{
    for (int i = 0; i < NTX; i++) {
        g_tx[i].addr   = g_txbuf_phys[i];
        g_tx[i].status = TXD_STAT_DD;   /* all slots free to begin with */
        g_tx[i].cmd    = 0;
    }
    reg_write(E1000_TDBAL, (uint32_t)(g_tx_phys & 0xFFFFFFFF));
    reg_write(E1000_TDBAH, (uint32_t)(g_tx_phys >> 32));
    reg_write(E1000_TDLEN, NTX * (uint32_t)sizeof(tx_desc_t));
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);
    g_tx_tail = 0;

    reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP |
                          (0x10u << TCTL_CT_SHIFT) |
                          (0x40u << TCTL_COLD_SHIFT));
    /* IPGT=10, IPGR1=8, IPGR2=6: the IEEE 802.3 standard gap. */
    reg_write(E1000_TIPG, 0x0060200A);
}

/* ---- public API ------------------------------------------------------ */
int e1000_present(void) { return g_ok; }
const uint8_t *e1000_mac(void) { return g_mac; }
int e1000_link_up(void) { return g_ok && (reg_read(E1000_STATUS) & STATUS_LU); }

void e1000_stats(uint64_t *tx_frames, uint64_t *rx_frames, uint64_t *irqs)
{
    if (tx_frames) *tx_frames = g_tx_frames;
    if (rx_frames) *rx_frames = g_rx_frames;
    if (irqs)      *irqs      = g_irqs;
}

int e1000_init(void)
{
    const pci_dev_t *d = pci_find(PCI_VENDOR_INTEL, E1000_DEV_82540EM);
    if (!d) {
        dbg_puts("E1000: no 8086:100E on the bus, skipping\r\n");
        return 0;
    }

    pci_enable(d);                       /* memory space + BUS MASTER */
    uint64_t base = pci_map_bar(d, 0);
    if (!base) {
        dbg_puts("E1000: BAR0 is not a mappable memory region\r\n");
        return 0;
    }
    g_regs = (volatile uint8_t *)base;
    g_irq  = d->irq_line;

    /* Reset: mask every interrupt first so the reset cannot raise one into
     * an IDT that has no handler for this device yet. */
    reg_write(E1000_IMC, 0xFFFFFFFF);
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);
    spin(1000);
    for (int i = 0; i < 1000 && (reg_read(E1000_CTRL) & CTRL_RST); i++)
        spin(10);
    reg_write(E1000_IMC, 0xFFFFFFFF);
    (void)reg_read(E1000_ICR);           /* reading ICR clears it */

    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_SLU);

    /* MAC: from the EEPROM if it answers, else from whatever the card
     * already latched into RAL/RAH. */
    uint16_t w0, w1, w2;
    if (eeprom_read(0, &w0) && eeprom_read(1, &w1) && eeprom_read(2, &w2)) {
        g_mac[0] = (uint8_t)w0; g_mac[1] = (uint8_t)(w0 >> 8);
        g_mac[2] = (uint8_t)w1; g_mac[3] = (uint8_t)(w1 >> 8);
        g_mac[4] = (uint8_t)w2; g_mac[5] = (uint8_t)(w2 >> 8);
    } else {
        uint32_t ral = reg_read(E1000_RAL0), rah = reg_read(E1000_RAH0);
        g_mac[0] = (uint8_t)ral;        g_mac[1] = (uint8_t)(ral >> 8);
        g_mac[2] = (uint8_t)(ral >> 16); g_mac[3] = (uint8_t)(ral >> 24);
        g_mac[4] = (uint8_t)rah;        g_mac[5] = (uint8_t)(rah >> 8);
    }

    /* Tell the card which unicast address is ours (bit 31 = address valid). */
    reg_write(E1000_RAL0, (uint32_t)g_mac[0] | ((uint32_t)g_mac[1] << 8) |
                          ((uint32_t)g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24));
    reg_write(E1000_RAH0, (uint32_t)g_mac[4] | ((uint32_t)g_mac[5] << 8) |
                          (1u << 31));

    /* An uninitialised multicast filter is a random filter. */
    for (int i = 0; i < 128; i++)
        reg_write(E1000_MTA + i * 4, 0);

    /* Rings: one page each, comfortably larger than NRX/NTX descriptors and
     * automatically 4 KiB aligned, which satisfies the card's 16-byte
     * alignment requirement several times over. */
    g_rx_phys = pmm_alloc_zeroed();
    g_tx_phys = pmm_alloc_zeroed();
    if (!g_rx_phys || !g_tx_phys || !alloc_buffers()) {
        dbg_puts("E1000: out of memory building the rings\r\n");
        return 0;
    }
    g_rx = (volatile rx_desc_t *)pmm_virt(g_rx_phys);
    g_tx = (volatile tx_desc_t *)pmm_virt(g_tx_phys);

    rx_init();
    tx_init();

    /* Only now that a ring exists is it safe to let the card interrupt us. */
    if (g_irq < 16) {
        irq_install(g_irq, e1000_irq);
        reg_write(E1000_IMS, ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO |
                             ICR_RXT0);
    }

    g_ok = 1;
    dbg_puts("E1000: 8086:100E up, mac ");
    for (int i = 0; i < 6; i++) {
        dbg_puts_hexn(g_mac[i], 2);
        if (i != 5) dbg_puts(":");
    }
    dbg_puts(", irq ");
    dbg_puts_dec(g_irq);
    dbg_puts(", link ");
    dbg_puts(e1000_link_up() ? "up" : "down");
    dbg_puts("\r\n");
    return 1;
}

int e1000_send(const void *frame, uint16_t len)
{
    if (!g_ok || len == 0 || len > BUFSZ)
        return 0;

    unsigned i = g_tx_tail;
    volatile tx_desc_t *d = &g_tx[i];

    /* The slot must have come back from the card before we reuse it. */
    for (int n = 0; n < 100000 && !(d->status & TXD_STAT_DD); n++)
        io_delay();
    if (!(d->status & TXD_STAT_DD))
        return 0;

    memcpy(g_txbuf[i], frame, len);
    d->addr   = g_txbuf_phys[i];
    d->length = len;
    d->cso    = 0;
    d->css    = 0;
    d->special = 0;
    d->status = 0;
    d->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

    g_tx_tail = (i + 1) % NTX;
    reg_write(E1000_TDT, g_tx_tail);

    /* RS made the card promise to write DD back when the frame is gone. */
    for (int n = 0; n < 100000; n++) {
        if (d->status & TXD_STAT_DD) {
            g_tx_frames++;
            return 1;
        }
        io_delay();
    }
    return 0;
}

uint16_t e1000_recv(void *buf, uint16_t max)
{
    if (!g_ok)
        return 0;

    volatile rx_desc_t *d = &g_rx[g_rx_head];
    if (!(d->status & RXD_STAT_DD))
        return 0;                        /* card has not filled this one */

    uint16_t len = d->length;
    if (len > max)
        len = max;
    memcpy(buf, g_rxbuf[g_rx_head], len);

    d->status = 0;
    /* Give the descriptor back: the tail is the last slot the card owns. */
    reg_write(E1000_RDT, g_rx_head);
    g_rx_head = (g_rx_head + 1) % NRX;
    g_rx_frames++;
    return len;
}

/* Poll the ring for up to `tries` spins.  Returns the frame length or 0. */
static uint16_t recv_wait(void *buf, uint16_t max, int tries)
{
    for (int n = 0; n < tries; n++) {
        uint16_t len = e1000_recv(buf, max);
        if (len)
            return len;
        io_delay();
    }
    return 0;
}

/* When a NIC "does nothing", the answer is almost always in these registers.
 * Printing them beats guessing, and the dump stays in the tree because the
 * next person to touch the ring code will want it too. */
static void dump_state(void)
{
    dbg_puts("       STATUS=0x"); dbg_puts_hexn(reg_read(E1000_STATUS), 8);
    dbg_puts(" CTRL=0x");   dbg_puts_hexn(reg_read(E1000_CTRL), 8);
    dbg_puts(" BMCR=0x");   dbg_puts_hexn(phy_read(PHY_BMCR), 4);
    dbg_puts("\r\n       RCTL=0x"); dbg_puts_hexn(reg_read(E1000_RCTL), 8);
    dbg_puts(" RDH=");      dbg_puts_dec(reg_read(E1000_RDH));
    dbg_puts(" RDT=");      dbg_puts_dec(reg_read(E1000_RDT));
    dbg_puts(" rx[0].sta=0x"); dbg_puts_hexn(g_rx[0].status, 2);
    dbg_puts("\r\n       TCTL=0x"); dbg_puts_hexn(reg_read(E1000_TCTL), 8);
    dbg_puts(" TDH=");      dbg_puts_dec(reg_read(E1000_TDH));
    dbg_puts(" TDT=");      dbg_puts_dec(reg_read(E1000_TDT));
    dbg_puts(" ICR=0x");    dbg_puts_hexn(reg_read(E1000_ICR), 8);
    dbg_puts("\r\n       RDBA=0x"); dbg_puts_hexn(
        ((uint64_t)reg_read(E1000_RDBAH) << 32) | reg_read(E1000_RDBAL), 16);
    dbg_puts(" ring@0x");   dbg_puts_hexn(g_rx_phys, 16);
    dbg_puts(" RDLEN=");    dbg_puts_dec(reg_read(E1000_RDLEN));
    dbg_puts("\r\n       TPT=");  dbg_puts_dec(reg_read(E1000_TPT));
    dbg_puts(" TPR=");      dbg_puts_dec(reg_read(E1000_TPR));
    dbg_puts(" GPRC=");     dbg_puts_dec(reg_read(E1000_GPRC));
    dbg_puts(" MPC=");      dbg_puts_dec(reg_read(E1000_MPC));
    dbg_puts(" RNBC=");     dbg_puts_dec(reg_read(E1000_RNBC));
    dbg_puts(" CRCERRS=");  dbg_puts_dec(reg_read(E1000_CRCERRS));
    dbg_puts("\r\n");
}

/* ---- self-test 1: PHY loopback --------------------------------------- */
static int test_loopback(void)
{
    static const char payload[] = "GNOS-E1000-LOOPBACK";
    static uint8_t got[BUFSZ];        /* static: 2 KiB is a lot of boot stack */
    uint8_t frame[64];

    phy_write(PHY_BMCR, (uint16_t)(phy_read(PHY_BMCR) | BMCR_LOOPBACK));
    spin(2000);

    memset(frame, 0, sizeof frame);
    memcpy(frame + 0, g_mac, 6);              /* destination: ourselves */
    memcpy(frame + 6, g_mac, 6);              /* source */
    frame[12] = 0x88; frame[13] = 0xB5;       /* IEEE local experimental #1 */
    memcpy(frame + 14, payload, sizeof payload);

    int ok = 0;
    if (!e1000_send(frame, sizeof frame)) {
        dbg_puts("E1000: loopback FAIL (transmit never completed)\r\n");
        dump_state();
    } else {
        uint16_t len = recv_wait(got, sizeof got, 200000);
        if (!len) {
            dbg_puts("E1000: loopback FAIL (nothing came back)\r\n");
            dump_state();
        } else if (len < 14 + (uint16_t)sizeof payload ||
                   memcmp(got + 14, payload, sizeof payload) != 0) {
            dbg_puts("E1000: loopback FAIL (frame corrupted, len=");
            dbg_puts_dec(len);
            dbg_puts(")\r\n");
        } else {
            dbg_puts("E1000: loopback PASS, ");
            dbg_puts_dec(len);
            dbg_puts(" bytes round-tripped through the PHY\r\n");
            ok = 1;
        }
    }

    phy_write(PHY_BMCR, (uint16_t)(phy_read(PHY_BMCR) & ~BMCR_LOOPBACK));
    spin(2000);
    return ok;
}

/* ---- self-test 2: a real ARP exchange -------------------------------- */
/*
 * Loopback proves the rings work; it does not prove a frame can leave the
 * machine.  ARP is the smallest exchange that does: broadcast "who has
 * 10.0.2.2", and the thing at the other end of the virtual wire -- QEMU's
 * user-mode network stack, which owns that address -- answers with its MAC.
 * Nothing here is a protocol stack; the frame is 42 bytes written by hand.
 */
#define ETHERTYPE_ARP 0x0806

static const uint8_t IP_SELF[4] = { 10, 0, 2, 15 };   /* QEMU hands us .15 */
static const uint8_t IP_GW[4]   = { 10, 0, 2,  2 };   /* the gateway/host */

static int test_arp(void)
{
    static uint8_t got[BUFSZ];
    uint8_t f[42];

    if (!e1000_link_up()) {
        dbg_puts("E1000: ARP skipped (no link)\r\n");
        return 1;                         /* not a driver bug */
    }

    memset(f, 0, sizeof f);
    memset(f + 0, 0xFF, 6);               /* dst: broadcast */
    memcpy(f + 6, g_mac, 6);              /* src: us */
    f[12] = ETHERTYPE_ARP >> 8; f[13] = ETHERTYPE_ARP & 0xFF;
    f[14] = 0x00; f[15] = 0x01;           /* htype: ethernet */
    f[16] = 0x08; f[17] = 0x00;           /* ptype: IPv4 */
    f[18] = 6;                            /* hardware address length */
    f[19] = 4;                            /* protocol address length */
    f[20] = 0x00; f[21] = 0x01;           /* oper: request */
    memcpy(f + 22, g_mac, 6);             /* sender hardware address */
    memcpy(f + 28, IP_SELF, 4);           /* sender protocol address */
    /* target hardware address stays zero -- that is what we are asking for */
    memcpy(f + 38, IP_GW, 4);             /* target protocol address */

    if (!e1000_send(f, sizeof f)) {
        dbg_puts("E1000: ARP FAIL (request never left the card)\r\n");
        dump_state();
        return 0;
    }

    /* The reply comes back through the host's event loop, so it takes far
     * longer than a loopback frame; be patient before declaring failure. */
    for (int round = 0; round < 40; round++) {
        uint16_t len = recv_wait(got, sizeof got, 20000);
        if (!len)
            continue;
        if (len < 42)
            continue;
        uint16_t et = (uint16_t)((got[12] << 8) | got[13]);
        if (et != ETHERTYPE_ARP)
            continue;                     /* something else on the wire */
        if (got[20] != 0x00 || got[21] != 0x02)
            continue;                     /* not a reply */
        if (memcmp(got + 28, IP_GW, 4) != 0)
            continue;                     /* not the gateway answering */

        dbg_puts("E1000: ARP PASS, 10.0.2.2 is at ");
        for (int i = 0; i < 6; i++) {
            dbg_puts_hexn(got[22 + i], 2);
            if (i != 5) dbg_puts(":");
        }
        dbg_puts("\r\n");
        return 1;
    }

    dbg_puts("E1000: ARP FAIL (no reply from 10.0.2.2)\r\n");
    dump_state();
    return 0;
}

/*
 * Both self-tests need the receiver to actually work, and under QEMU it does
 * not work for the first second after RCTL is written.
 *
 * QEMU arms a one-second "flush queue" timer on every write to RCTL, and
 * while that timer is pending e1000_can_receive() reports false and
 * e1000_receive_iov() returns immediately -- every inbound frame is dropped
 * without a trace, without a statistic, and without touching the ring.  The
 * intent is to give a guest that enables the receiver before it has finished
 * building descriptors a moment to catch up.  The effect on a driver that
 * builds the ring *first* and then tests it is a receiver that is silently
 * dead exactly when the test runs.
 *
 * Real 82540EMs have no such window, so this wait is emulator etiquette
 * rather than hardware programming -- but it costs one second once, and
 * without it the tests below report a nonexistent bug.
 */
static void wait_out_rx_grace_period(void)
{
    timer_delay_ms(1100);
}

int e1000_selftest(void)
{
    if (!g_ok)
        return 0;

    wait_out_rx_grace_period();

    int a = test_loopback();
    int b = test_arp();

    dbg_puts("E1000: tx=");
    dbg_puts_dec((uint32_t)g_tx_frames);
    dbg_puts(" rx=");
    dbg_puts_dec((uint32_t)g_rx_frames);
    dbg_puts(" frames\r\n");
    return a && b;
}
