/*
 * ata.h — legacy ATA (IDE) disks over programmed I/O. (GPLv2)
 *
 * This is the first real block device in GNOS.  Everything before it -- the
 * ext2 root, the FAT volume -- lived in the initrd the bootloader dropped in
 * RAM, which meant "writable until you switch the machine off".  A disk is
 * what makes an installer possible at all.
 *
 * The driver deliberately uses the oldest interface the PC has: the fixed
 * legacy port pairs at 0x1F0/0x3F6 and 0x170/0x376, programmed I/O, no DMA,
 * no interrupts, polling for completion.  That costs throughput -- every
 * sector crosses the bus 16 bits at a time through the CPU -- and buys three
 * things worth more here:
 *
 *   - no PCI enumeration, no bus-mastering setup, no scatter/gather lists;
 *   - no interrupt handler, so nothing to get wrong about IRQ 14/15 routing
 *     through the legacy PIC or the IOAPIC;
 *   - it works on the plainest QEMU invocation, with no -device at all.
 *
 * The disks appear as /dev/sda, /dev/sdb, ... and their MBR partitions as
 * /dev/sda1 .. /dev/sda4.  A partition node is a *window*: the same driver,
 * the same disk, with an offset added and the length clamped, which is all a
 * partition has ever been.
 */
#ifndef GNOS_ATA_H
#define GNOS_ATA_H

#include <stdint.h>

/* Probe both channels, publish /dev/sd* and their partitions, and register
 * with the subsystem table.  Returns the number of disks found.  Safe to
 * call more than once: only the first pass probes anything. */
int ata_init(void);

/* Whole-disk boot read: copy the first used partition of disk 0 into dst
 * (cap bytes).  Returns the byte count, or 0 on no-disk / no-partition /
 * too-small buffer / I/O error. */
int ata_read_boot_partition(uint8_t *dst, uint32_t cap);

/* Sector size.  Every ATA disk the legacy interface can address uses 512-byte
 * logical sectors; the 4Kn drives that do not are NVMe-era hardware that
 * never speaks this protocol. */
#define ATA_SECTOR 512

#endif /* GNOS_ATA_H */
