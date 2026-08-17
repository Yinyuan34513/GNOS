/*
 * acpi.h — ACPI table discovery. (GPLv2)
 *
 * This is the *table* half of ACPI only: find the RSDP, walk the RSDT/XSDT,
 * validate every checksum, and remember where each table lives so later code
 * can ask for one by signature.  There is no AML interpreter here and there
 * will not be one -- the DSDT is located and reported, not executed.
 *
 * That still buys the things a kernel needs early:
 *   FACP (FADT)  the PM1a control block, which is how you power the machine
 *                off, and the reset register.
 *   APIC (MADT)  how many CPUs there are and where the LAPIC/IOAPIC MMIO is,
 *                which is the prerequisite for ever leaving the 8259 PIC.
 *   HPET         a monotonic timer that does not drift like the PIT.
 *
 * Everything is read-only and parsed once at boot, so nothing here can fail
 * in a way that matters later: a machine with no ACPI at all simply reports
 * zero tables and the kernel carries on with the PIT and the PIC.
 */
#ifndef GNUCOS_ACPI_H
#define GNUCOS_ACPI_H

#include <stdint.h>

#define ACPI_MAX_TABLES 32

/* The 36-byte header every ACPI table starts with. */
typedef struct {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;

/*
 * Discover and index the ACPI tables.  `rsdp` is the pointer the bootloader
 * handed us (already a mapped virtual address), or 0 to fall back to scanning
 * the EBDA and the 0xE0000-0xFFFFF BIOS area for the "RSD PTR " signature.
 * Returns the number of tables indexed, 0 if the machine has no usable ACPI.
 */
int acpi_init(uint64_t rsdp);

/* The table with this 4-character signature ("FACP", "APIC", "HPET", ...),
 * or NULL.  The pointer is into the firmware's tables and stays valid. */
const acpi_sdt_t *acpi_find(const char *sig);

/* How many tables were indexed, and read-only access to one of them. */
int                acpi_table_count(void);
const acpi_sdt_t  *acpi_table(int i);

/* Facts extracted from the tables at init time.  Each is 0 when the relevant
 * table was absent, so a caller can treat 0 as "fall back to the legacy way"
 * without a second "is ACPI present" flag. */
int      acpi_cpu_count(void);      /* enabled local APICs in the MADT */
uint64_t acpi_lapic_base(void);     /* physical address of the local APIC */
uint64_t acpi_ioapic_base(void);    /* physical address of the first IOAPIC */
uint64_t acpi_hpet_base(void);      /* physical address of the HPET block */
uint16_t acpi_pm1a_control(void);   /* PM1a control port, for poweroff */

/* Print the table index to the debug console, one line per table. */
void acpi_dump(void);

/* Arm the ACPI power button: enable the PM1a PWRBTN event and route the SCI
 * (IRQ 9) to a handler that powers the machine off when the button fires.
 * Safe to call once, after acpi_init(). */
void acpi_pm1_init(void);

/* Request a hard power-off: write SLP_TYP(S5) | SLP_EN to the PM1a control
 * block.  Never returns on success (the machine is gone); on failure it
 * writes a diagnostic and parks the CPU. */
void acpi_poweroff(void);

/* Non-fatal self check: re-validate the checksums, confirm the tables we
 * indexed can still be found by signature, and confirm the MADT-derived CPU
 * count is sane.  Prints one line to the debug console. */
void acpi_self_test(void);

#endif
