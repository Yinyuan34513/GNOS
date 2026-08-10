/*
 * e1000.h — Intel 82540EM ("e1000") gigabit ethernet driver. (GPLv2)
 *
 * This is the card QEMU gives you for `-device e1000`, and it is the classic
 * teaching NIC: no firmware to load, no mailbox protocol, just two rings of
 * 16-byte descriptors in main memory that the card walks by itself.
 *
 * The model to keep in mind is a pair of circular conveyor belts:
 *
 *      software                      hardware
 *      --------                      --------
 *   TX: writes descriptor at TAIL --> card consumes up to TAIL, moves HEAD
 *   RX: hands empty buffers up to TAIL, card fills them and moves HEAD
 *
 * so for transmit, HEAD == TAIL means "card is idle", and for receive the
 * descriptors between HEAD and TAIL are the ones the card is allowed to fill.
 * Everything else in this driver is just getting the card out of reset and
 * telling it where those two belts live.
 *
 * There is no protocol stack above this: send and receive deal in whole
 * ethernet frames, destination MAC first.  That is deliberate -- the point is
 * to show DMA, descriptor rings and device interrupts, not to reimplement
 * TCP.  The one exception is the self-test, which hand-builds a single ARP
 * request, because ARP is the smallest real protocol exchange that proves
 * frames are leaving the machine and coming back.
 */
#ifndef GNOS_E1000_H
#define GNOS_E1000_H

#include <stdint.h>

/* Locate the card, reset it, build the rings, hook its IRQ.  1 on success. */
int e1000_init(void);

/* True once e1000_init() has succeeded. */
int e1000_present(void);

/* Our MAC address, six bytes.  Only meaningful after a successful init. */
const uint8_t *e1000_mac(void);

/* Live link state, straight out of the STATUS register. */
int e1000_link_up(void);

/*
 * Transmit one complete ethernet frame (dst MAC, src MAC, ethertype, payload)
 * and wait for the card to acknowledge it.  Returns 1 on success.
 */
int e1000_send(const void *frame, uint16_t len);

/*
 * Take one received frame, if any is waiting.  Returns its length in bytes,
 * or 0 when the ring is empty.  Never blocks.
 */
uint16_t e1000_recv(void *buf, uint16_t max);

/* Counters, for whoever wants to print them.  Any pointer may be NULL. */
void e1000_stats(uint64_t *tx_frames, uint64_t *rx_frames, uint64_t *irqs);

/*
 * Two self-tests, both headless and both falsifiable:
 *   1. put the PHY in loopback and check a known frame comes back;
 *   2. take it out of loopback and ARP the gateway, which proves the frame
 *      actually reached the outside world and something answered.
 * Prints the verdict for each.  Returns 1 only if both pass.
 */
int e1000_selftest(void);

#endif
