/*
 * pci.c — minimal PCI bus enumeration. (GPLv2)
 *
 * See pci.h for the type-1 access scheme.  We only scan bus 0, which is where
 * QEMU places every device we ask for (-device e1000, -device ac97).  Each
 * function found is recorded with its identity and raw BARs; the drivers do
 * the rest.
 */
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "debugcon.h"

#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC

pci_dev_t g_pci_devs[PCI_MAX_DEVICES];
int       g_pci_count;

static uint32_t cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (reg & 0xFC);
    outl(PCI_ADDR_PORT, addr);
    return inl(PCI_DATA_PORT);
}

static void cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg,
                        uint32_t val)
{
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
                    (reg & 0xFC);
    outl(PCI_ADDR_PORT, addr);
    outl(PCI_DATA_PORT, val);
}

const pci_dev_t *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < g_pci_count; i++)
        if (g_pci_devs[i].vendor == vendor && g_pci_devs[i].device == device)
            return &g_pci_devs[i];
    return NULL;
}

const pci_dev_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < g_pci_count; i++)
        if (g_pci_devs[i].class_code == class_code &&
            g_pci_devs[i].subclass == subclass)
            return &g_pci_devs[i];
    return NULL;
}

static uint64_t bar_phys_and_size(const pci_dev_t *d, int idx, uint64_t *size_out)
{
    uint32_t lo = d->bar[idx];
    *size_out = 0;
    if (lo & 0x1)
        return 0;                       /* I/O BAR: not memory-mapped */

    uint8_t  reg  = (uint8_t)(0x10 + idx * 4);
    uint32_t orig = cfg_read32(d->bus, d->dev, d->fn, reg);

    /* Sizing moves the BAR while it is in progress, so decoding has to be off
     * or the device briefly answers at an address nobody asked about. */
    uint32_t cmd = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd & ~0x3u);

    /* The size is the number of low-order address bits the device hard-wires
     * to zero, learned by writing all-ones and seeing what sticks. */
    cfg_write32(d->bus, d->dev, d->fn, reg, 0xFFFFFFFF);
    uint32_t rb = cfg_read32(d->bus, d->dev, d->fn, reg);
    cfg_write32(d->bus, d->dev, d->fn, reg, orig);
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);

    uint64_t base = (uint64_t)lo & ~0xFULL;
    uint32_t mask = rb & 0xFFFFFFF0u;
    if (mask == 0)
        return base;                    /* BAR not implemented: size stays 0 */

    /* Keep this arithmetic in 32 bits.  Complementing the mask *after* it has
     * been widened to 64 sets all 32 upper bits, and the resulting "size" of
     * ~2^52 pages will happily consume every free frame in the machine
     * building page tables before anything notices. */
    uint64_t size = (uint64_t)(uint32_t)(~mask) + 1;

    /* 64-bit BAR: the next config dword holds the high 32 bits of the address.
     * We do not probe the high half for size -- nothing QEMU gives us needs a
     * region larger than 4 GiB, and a wrong size here would map the world. */
    if ((lo & 0x7) == 0x4)
        base |= ((uint64_t)d->bar[idx + 1]) << 32;

    *size_out = size;
    return base;
}

uint16_t pci_bar_io(const pci_dev_t *d, int idx)
{
    if (idx < 0 || idx > 5)
        return 0;
    uint32_t v = d->bar[idx];
    if (!(v & 0x1))
        return 0;                       /* memory BAR, not an I/O one */
    return (uint16_t)(v & ~0x3u);
}

void pci_enable(const pci_dev_t *d)
{
    uint32_t cmd = cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x7;                 /* I/O space | memory space | bus master */
    cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

uint64_t pci_map_bar(const pci_dev_t *d, int idx)
{
    if (idx < 0 || idx > 5)
        return 0;
    uint64_t size;
    uint64_t phys = bar_phys_and_size(d, idx, &size);
    if (!phys || !size)
        return 0;
    return vmm_map_mmio(phys, size);
}

void pci_init(void)
{
    g_pci_count = 0;

    for (uint8_t bus = 0; bus < 1; bus++) {          /* bus 0 only */
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = cfg_read32(bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF)
                    continue;           /* no device here */

                if (g_pci_count >= PCI_MAX_DEVICES)
                    break;

                pci_dev_t *d = &g_pci_devs[g_pci_count];
                uint32_t cls = cfg_read32(bus, dev, fn, 0x08);
                d->bus      = bus;
                d->dev      = dev;
                d->fn       = fn;
                d->vendor   = vendor;
                d->device   = (uint16_t)(id >> 16);
                d->progif   = (uint8_t)(cls >> 8);
                d->subclass = (uint8_t)(cls >> 16);
                d->class_code = (uint8_t)(cls >> 24);
                d->hdr_type = (uint8_t)(cfg_read32(bus, dev, fn, 0x0C) >> 16);
                for (int i = 0; i < 6; i++)
                    d->bar[i] = cfg_read32(bus, dev, fn, (uint8_t)(0x10 + i * 4));
                uint32_t il = cfg_read32(bus, dev, fn, 0x3C);
                d->irq_line = (uint8_t)(il & 0xFF);
                d->irq_pin  = (uint8_t)((il >> 8) & 0xFF);
                g_pci_count++;

                /* Stop after fn 0 if the device is not multifunction. */
                if (fn == 0) {
                    uint8_t hdr = d->hdr_type;
                    if (!(hdr & 0x80)) {
                        fn = 7;        /* break out of the fn loop */
                    }
                }
            }
        }
    }

    dbg_puts("PCI: scanned bus 0, ");
    dbg_puts_dec((uint32_t)g_pci_count);
    dbg_puts(" device(s):\r\n");
    for (int i = 0; i < g_pci_count; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        dbg_puts("  ");
        dbg_puts_hexn(d->bus, 2); dbg_puts(":");
        dbg_puts_hexn(d->dev, 2); dbg_puts(".");
        dbg_puts_hexn(d->fn, 1);  dbg_puts("  ");
        dbg_puts_hexn(d->vendor, 4); dbg_puts(":");
        dbg_puts_hexn(d->device, 4); dbg_puts("  class ");
        dbg_puts_hexn(d->class_code, 2); dbg_puts("/");
        dbg_puts_hexn(d->subclass, 2); dbg_puts("\r\n");
    }
}
