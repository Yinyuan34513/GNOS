/*
 * acpi.c — ACPI table discovery. (GPLv2)
 *
 * See acpi.h.  The awkward parts of this format, and how they are handled:
 *
 *   - There are two root tables.  ACPI 1.0 has the RSDT, an array of 32-bit
 *     physical pointers; ACPI 2.0+ adds the XSDT, the same thing with 64-bit
 *     pointers.  The RSDP tells you which, via its revision field, and the
 *     spec says to prefer the XSDT when it is there because on a machine with
 *     tables above 4 GiB the RSDT physically cannot describe them.
 *   - The RSDP has *two* checksums: the first covers the original 20 bytes,
 *     the second the whole (longer) 2.0 structure.  Checking only the first
 *     on a 2.0 RSDP would accept a table whose 64-bit XSDT pointer is
 *     corrupt, which is exactly the field we are about to dereference.
 *   - Every pointer inside the tables is *physical*.  Limine direct-maps all
 *     of RAM at the HHDM offset, so pmm_virt() turns each one into something
 *     we can read; nothing here needs its own page mapping.
 */
#include <stdint.h>
#include <stddef.h>

#include "acpi.h"
#include "pmm.h"
#include "kstring.h"
#include "debugcon.h"

extern uint64_t g_hhdm;

typedef struct {
    char     sig[8];            /* "RSD PTR " */
    uint8_t  checksum;          /* over the first 20 bytes */
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_addr;
    /* ACPI 2.0+ only, present when revision >= 2 */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;      /* over `length` bytes */
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

/* MADT (signature "APIC") */
typedef struct {
    acpi_sdt_t hdr;
    uint32_t   lapic_addr;
    uint32_t   flags;
    /* followed by variable-length entries */
} __attribute__((packed)) acpi_madt_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_t;

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_LAPIC_OVERRIDE 5
#define MADT_LAPIC_ENABLED  0x1

typedef struct {
    madt_entry_t hdr;
    uint8_t      acpi_id;
    uint8_t      apic_id;
    uint32_t     flags;
} __attribute__((packed)) madt_lapic_t;

typedef struct {
    madt_entry_t hdr;
    uint8_t      ioapic_id;
    uint8_t      reserved;
    uint32_t     addr;
    uint32_t     gsi_base;
} __attribute__((packed)) madt_ioapic_t;

typedef struct {
    madt_entry_t hdr;
    uint16_t     reserved;
    uint64_t     addr;
} __attribute__((packed)) madt_lapic_override_t;

/* FADT (signature "FACP") -- only the fields we use. */
typedef struct {
    acpi_sdt_t hdr;
    uint32_t   firmware_ctrl;
    uint32_t   dsdt;
    uint8_t    reserved0;
    uint8_t    preferred_pm_profile;
    uint16_t   sci_int;
    uint32_t   smi_cmd;
    uint8_t    acpi_enable;
    uint8_t    acpi_disable;
    uint8_t    s4bios_req;
    uint8_t    pstate_cnt;
    uint32_t   pm1a_evt_blk;
    uint32_t   pm1b_evt_blk;
    uint32_t   pm1a_cnt_blk;
    uint32_t   pm1b_cnt_blk;
} __attribute__((packed)) acpi_fadt_t;

/* HPET (signature "HPET") */
typedef struct {
    acpi_sdt_t hdr;
    uint32_t   event_timer_block_id;
    uint8_t    addr_space_id;
    uint8_t    register_bit_width;
    uint8_t    register_bit_offset;
    uint8_t    reserved;
    uint64_t   address;
} __attribute__((packed)) acpi_hpet_t;

static const acpi_sdt_t *g_tables[ACPI_MAX_TABLES];
static int      g_ntables;
static int      g_cpus;
static uint64_t g_lapic, g_ioapic, g_hpet;
static uint16_t g_pm1a_cnt;

/* ---- helpers ----------------------------------------------------------- */

static int checksum_ok(const void *p, uint32_t len)
{
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

static int sig_is(const char *sig4, const char *want)
{
    return sig4[0] == want[0] && sig4[1] == want[1] &&
           sig4[2] == want[2] && sig4[3] == want[3];
}

static void put_sig(const char *sig4)
{
    char tmp[5] = { sig4[0], sig4[1], sig4[2], sig4[3], 0 };
    dbg_puts(tmp);
}

/* Turn a physical table address into something we can read. */
static const acpi_sdt_t *sdt_at(uint64_t phys)
{
    if (!phys)
        return NULL;
    return (const acpi_sdt_t *)pmm_virt(phys);
}

/* ---- RSDP -------------------------------------------------------------- */

static const acpi_rsdp_t *validate_rsdp(const void *p)
{
    const acpi_rsdp_t *r = (const acpi_rsdp_t *)p;
    if (memcmp(r->sig, "RSD PTR ", 8) != 0)
        return NULL;
    if (!checksum_ok(r, 20))
        return NULL;
    /* A 2.0 RSDP is only trustworthy if the extended checksum agrees too --
     * and that is the one covering the XSDT pointer we are about to use. */
    if (r->revision >= 2 && (r->length < sizeof(acpi_rsdp_t) ||
                             !checksum_ok(r, r->length)))
        return NULL;
    return r;
}

/* Scan a physical range for the RSDP signature.  It is always 16-byte
 * aligned, which is what makes a brute-force scan cheap enough to be the
 * fallback when the bootloader did not tell us where it is. */
static const acpi_rsdp_t *scan_range(uint64_t phys_start, uint64_t phys_end)
{
    for (uint64_t p = phys_start; p + 20 <= phys_end; p += 16) {
        const acpi_rsdp_t *r = validate_rsdp(pmm_virt(p));
        if (r)
            return r;
    }
    return NULL;
}

static const acpi_rsdp_t *find_rsdp(uint64_t hint)
{
    if (hint) {
        /* Limine hands us a pointer that is already mapped, not a physical
         * address; only fall through to scanning if it does not check out. */
        const acpi_rsdp_t *r = validate_rsdp((const void *)(uintptr_t)hint);
        if (r)
            return r;
    }

    /* The EBDA segment address lives as a 16-bit paragraph count at 0x40E. */
    const uint16_t *bda = (const uint16_t *)pmm_virt(0x40E);
    uint64_t ebda = (uint64_t)(*bda) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        const acpi_rsdp_t *r = scan_range(ebda, ebda + 1024);
        if (r)
            return r;
    }
    return scan_range(0xE0000, 0x100000);
}

/* ---- table-specific extraction ----------------------------------------- */

static void parse_madt(const acpi_sdt_t *hdr)
{
    const acpi_madt_t *m = (const acpi_madt_t *)hdr;
    g_lapic = m->lapic_addr;

    const uint8_t *p   = (const uint8_t *)hdr + sizeof(acpi_madt_t);
    const uint8_t *end = (const uint8_t *)hdr + hdr->length;

    while (p + sizeof(madt_entry_t) <= end) {
        const madt_entry_t *e = (const madt_entry_t *)p;
        /* A zero-length entry would spin here forever; firmware bugs of
         * exactly this shape are why the check is not optional. */
        if (e->length < sizeof(madt_entry_t) || p + e->length > end)
            break;

        switch (e->type) {
        case MADT_LAPIC: {
            const madt_lapic_t *l = (const madt_lapic_t *)e;
            if (l->flags & MADT_LAPIC_ENABLED)
                g_cpus++;
            break;
        }
        case MADT_IOAPIC: {
            const madt_ioapic_t *io = (const madt_ioapic_t *)e;
            if (!g_ioapic)
                g_ioapic = io->addr;
            break;
        }
        case MADT_LAPIC_OVERRIDE: {
            /* When present this supersedes the 32-bit address in the header,
             * which is the only way a LAPIC above 4 GiB can be described. */
            const madt_lapic_override_t *o = (const madt_lapic_override_t *)e;
            g_lapic = o->addr;
            break;
        }
        }
        p += e->length;
    }
}

static void parse_fadt(const acpi_sdt_t *hdr)
{
    if (hdr->length < sizeof(acpi_fadt_t))
        return;
    const acpi_fadt_t *f = (const acpi_fadt_t *)hdr;
    g_pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
}

static void parse_hpet(const acpi_sdt_t *hdr)
{
    if (hdr->length < sizeof(acpi_hpet_t))
        return;
    const acpi_hpet_t *h = (const acpi_hpet_t *)hdr;
    g_hpet = h->address;
}

static void index_table(const acpi_sdt_t *t)
{
    if (!t || g_ntables >= ACPI_MAX_TABLES)
        return;
    /* A table whose checksum is wrong is worse than a missing one: acting on
     * its contents means acting on garbage.  Skip it and say so. */
    if (t->length < sizeof(acpi_sdt_t) || !checksum_ok(t, t->length)) {
        dbg_puts("ACPI: bad checksum on ");
        put_sig(t->sig);
        dbg_puts(", ignored\r\n");
        return;
    }

    g_tables[g_ntables++] = t;

    if (sig_is(t->sig, "APIC"))
        parse_madt(t);
    else if (sig_is(t->sig, "FACP"))
        parse_fadt(t);
    else if (sig_is(t->sig, "HPET"))
        parse_hpet(t);
}

/* ---- public interface --------------------------------------------------- */

int acpi_init(uint64_t rsdp)
{
    g_ntables = 0;
    g_cpus    = 0;
    g_lapic   = g_ioapic = g_hpet = 0;
    g_pm1a_cnt = 0;

    const acpi_rsdp_t *r = find_rsdp(rsdp);
    if (!r) {
        dbg_puts("ACPI: no RSDP found\r\n");
        return 0;
    }

    dbg_puts("ACPI: RSDP rev ");
    dbg_puts_dec(r->revision);
    dbg_puts(" at ");
    dbg_puts_hex((uint64_t)(uintptr_t)r - g_hhdm);
    dbg_puts("\r\n");

    /* Prefer the XSDT: on a machine with tables above 4 GiB the RSDT cannot
     * even express where they are. */
    const acpi_sdt_t *root = NULL;
    int xsdt = 0;
    if (r->revision >= 2 && r->xsdt_addr) {
        root = sdt_at(r->xsdt_addr);
        xsdt = 1;
    }
    if (!root || !sig_is(root->sig, "XSDT")) {
        root = sdt_at(r->rsdt_addr);
        xsdt = 0;
    }
    if (!root || root->length < sizeof(acpi_sdt_t) ||
        !checksum_ok(root, root->length)) {
        dbg_puts("ACPI: root table missing or corrupt\r\n");
        return 0;
    }

    index_table(root);

    uint32_t entries = (root->length - (uint32_t)sizeof(acpi_sdt_t)) /
                       (xsdt ? 8u : 4u);
    const uint8_t *arr = (const uint8_t *)root + sizeof(acpi_sdt_t);
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t phys;
        if (xsdt) {
            /* The array is only 4-byte aligned inside the table, so a
             * straight uint64_t load can be misaligned; build it by bytes. */
            uint64_t v = 0;
            memcpy(&v, arr + i * 8, 8);
            phys = v;
        } else {
            uint32_t v = 0;
            memcpy(&v, arr + i * 4, 4);
            phys = v;
        }
        index_table(sdt_at(phys));
    }

    /* The DSDT hangs off the FADT rather than the root table, so it is never
     * reached by the loop above.  Index it too: it is the one table whose
     * absence says the firmware is broken. */
    const acpi_sdt_t *fadt = acpi_find("FACP");
    if (fadt && fadt->length >= sizeof(acpi_fadt_t))
        index_table(sdt_at(((const acpi_fadt_t *)fadt)->dsdt));

    return g_ntables;
}

const acpi_sdt_t *acpi_find(const char *sig)
{
    if (!sig)
        return NULL;
    for (int i = 0; i < g_ntables; i++)
        if (sig_is(g_tables[i]->sig, sig))
            return g_tables[i];
    return NULL;
}

int               acpi_table_count(void) { return g_ntables; }
const acpi_sdt_t *acpi_table(int i)
{
    return (i >= 0 && i < g_ntables) ? g_tables[i] : NULL;
}

int      acpi_cpu_count(void)    { return g_cpus; }
uint64_t acpi_lapic_base(void)   { return g_lapic; }
uint64_t acpi_ioapic_base(void)  { return g_ioapic; }
uint64_t acpi_hpet_base(void)    { return g_hpet; }
uint16_t acpi_pm1a_control(void) { return g_pm1a_cnt; }

void acpi_dump(void)
{
    dbg_puts("ACPI: ");
    dbg_puts_dec((uint32_t)g_ntables);
    dbg_puts(" table(s)\r\n");
    for (int i = 0; i < g_ntables; i++) {
        dbg_puts("ACPI:   ");
        put_sig(g_tables[i]->sig);
        dbg_puts(" rev ");
        dbg_puts_dec(g_tables[i]->revision);
        dbg_puts(" len ");
        dbg_puts_dec(g_tables[i]->length);
        dbg_puts(" @");
        dbg_puts_hex((uint64_t)(uintptr_t)g_tables[i] - g_hhdm);
        dbg_puts("\r\n");
    }
    if (g_cpus) {
        dbg_puts("ACPI: ");
        dbg_puts_dec((uint32_t)g_cpus);
        dbg_puts(" CPU(s), LAPIC @");
        dbg_puts_hex(g_lapic);
        dbg_puts(", IOAPIC @");
        dbg_puts_hex(g_ioapic);
        dbg_puts("\r\n");
    }
    if (g_hpet) {
        dbg_puts("ACPI: HPET @");
        dbg_puts_hex(g_hpet);
        dbg_puts("\r\n");
    }
}

void acpi_self_test(void)
{
    dbg_puts("ACPI: self-test ... ");
    if (g_ntables == 0) {
        dbg_puts("skipped (no ACPI)\r\n");
        return;
    }

    int ok = 1;

    /* Every indexed table must still validate and be findable by its own
     * signature -- that is what proves the index and the memory agree. */
    for (int i = 0; i < g_ntables && ok; i++) {
        const acpi_sdt_t *t = g_tables[i];
        ok = ok && checksum_ok(t, t->length);
        ok = ok && (t->length >= sizeof(acpi_sdt_t));
        ok = ok && (acpi_find(t->sig) != NULL);
    }

    /* A machine with ACPI at all has an FADT; without one there is no DSDT
     * pointer and no power management, so its absence means we mis-parsed. */
    ok = ok && (acpi_find("FACP") != NULL);

    /* If there is a MADT it must describe at least the CPU we are running
     * on, and a LAPIC address that is not obviously nonsense. */
    if (ok && acpi_find("APIC")) {
        ok = ok && (g_cpus >= 1);
        ok = ok && (g_lapic != 0);
    }

    /* A lookup for a signature no firmware emits must miss. */
    ok = ok && (acpi_find("ZZZZ") == NULL);

    /* The header layout must match the spec, or every offset above is wrong. */
    ok = ok && (sizeof(acpi_sdt_t) == 36);

    dbg_puts(ok ? "ok\r\n" : "FAIL\r\n");
}
