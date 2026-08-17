/*
 * pci.h — minimal PCI bus enumeration. (GPLv2)
 *
 * Type-1 configuration access through ports 0xCF8/0xCFC: the address port
 * selects a (bus, device, function, register) tuple and the data port reads
 * or writes its 32-bit config word.  We scan bus 0 (where QEMU puts every
 * device) and record what we find so the E1000 and AC97 drivers can locate
 * themselves by vendor/device id without re-walking the bus.
 */
#ifndef GNOS_PCI_H
#define GNOS_PCI_H

#include <stdint.h>
#include <stddef.h>

#define PCI_MAX_DEVICES 16

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, progif;
    uint8_t  hdr_type;
    uint32_t bar[6];          /* raw BAR values as read from config space */
    uint8_t  irq_line, irq_pin;
} pci_dev_t;

extern pci_dev_t g_pci_devs[PCI_MAX_DEVICES];
extern int       g_pci_count;

/* Scan every (bus,dev,fn) on bus 0, fill g_pci_devs.  Safe to call once. */
void pci_init(void);

/* Find the first device matching a vendor/device pair, or NULL. */
const pci_dev_t *pci_find(uint16_t vendor, uint16_t device);

/*
 * Find the first device of a given class/subclass, or NULL.  Some devices are
 * better identified by what they *are* than by who made them: QEMU's HD Audio
 * controller is 8086:2668 as `intel-hda` and 8086:293E as `ich9-intel-hda`,
 * but both are class 04 subclass 03 and both are driven by the same code.
 */
const pci_dev_t *pci_find_class(uint8_t class_code, uint8_t subclass);

/*
 * Map memory-type BAR `idx` of device `d` into the kernel address space with
 * the uncacheable attribute and return its virtual base.  Returns 0 for I/O
 * BARs or unmapped regions.  The mapping size is discovered by the standard
 * "write all-ones, read back, restore" trick.
 */
uint64_t pci_map_bar(const pci_dev_t *d, int idx);

/*
 * Set I/O space, memory space and bus-master enable in the command register.
 * Bus mastering is not optional for the E1000: without it the card cannot
 * fetch its own descriptors and every transmit silently does nothing.
 */
void pci_enable(const pci_dev_t *d);

/*
 * Base I/O port of BAR `idx`, or 0 if that BAR is memory-mapped instead.
 * Not every device moved to MMIO: QEMU's AC97 still exposes both of its
 * register banks in I/O space, the way ICH chipsets always did.
 */
uint16_t pci_bar_io(const pci_dev_t *d, int idx);

/* Class codes we care about. */
#define PCI_CLASS_NETWORK   0x02
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_SUBCLASS_AUDIO   0x01   /* AC'97 and older: "multimedia audio" */
#define PCI_SUBCLASS_HDA     0x03   /* Intel High Definition Audio */

/* A few well-known ids. */
#define PCI_VENDOR_INTEL 0x8086
#define E1000_DEV_82540EM 0x100E   /* QEMU `-device e1000` */
#define AC97_DEV_82801AA  0x2415   /* QEMU `-device ac97` (Intel ICH AC97) */

#endif
