/*
 * hid-list.c
 *
 * Lists all USB HID devices using hidapi, shows the full report
 * descriptor structure (collections, input/output/feature reports with
 * report IDs and items) and probes feature/input reports.
 *
 * Build (libusb backend, no kernel HID/INPUT support required):
 *   gcc -Wall -O2 hid-list.c -lhidapi-libusb -o hid-list
 *
 * or (hidraw backend, requires kernel CONFIG_HIDRAW=y):
 *   gcc -Wall -O2 hid-list.c -lhidapi-hidraw -o hid-list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>
#include <hidapi/hidapi.h>

#define MAX_FEATURE_ID   31
#define FEATURE_BUF_SIZE 256
#define MAX_USAGES       64
#define MAX_DESC         4096

static void print_wstr(const wchar_t *s)
{
    if (!s) {
        printf("(null)");
        return;
    }

    char buf[1024];

    if (wcstombs(buf, s, sizeof(buf) - 1) == (size_t)-1)
        printf("(conversion error)");
    else
        printf("%s", buf);
}

static void print_hex(const unsigned char *data, int len)
{
    int i;
    for (i = 0; i < len && i < 32; i++)
        printf("%02X ", data[i]);
    if (len > 32)
        printf("...");
}

/* Probes feature reports for report IDs 0..MAX_FEATURE_ID */
static void probe_feature_reports(hid_device *handle)
{
    unsigned char buf[FEATURE_BUF_SIZE];
    int found = 0;

    for (int rid = 0; rid <= MAX_FEATURE_ID; rid++) {
        memset(buf, 0, sizeof(buf));
        buf[0] = (unsigned char)rid;

        int res = hid_get_feature_report(handle, buf, sizeof(buf));
        if (res > 0) {
            if (!found) {
                printf("    Feature reports:\n");
                found = 1;
            }
            printf("      [ID 0x%02X] %d bytes: ", rid, res);
            print_hex(buf, res);
            printf("\n");
        }
    }
    if (!found)
        printf("    Feature reports: none detected\n");
}

/* Probes input reports via non-blocking read */
static void probe_input_report(hid_device *handle)
{
    unsigned char buf[FEATURE_BUF_SIZE];

    hid_set_nonblocking(handle, 1);

    int res = hid_read(handle, buf, sizeof(buf));
    if (res > 0) {
        printf("    Input report (pending): %d bytes: ", res);
        print_hex(buf, res);
        printf("\n");
    } else {
        printf("    Input reports: none pending\n");
    }

    hid_set_nonblocking(handle, 0);
}

/* ────────────────────────────────────────────────────────────
 * HID report descriptor parsing (HID 1.11) -> WebHID-like
 * structure, matching the webhid-explorer frontend parser.
 * ──────────────────────────────────────────────────────────── */

typedef struct {
    unsigned report_size;
    unsigned report_count;
    unsigned char data_flags;   /* Input/Output/Feature flags */
    int is_range;
    unsigned long usages[MAX_USAGES];
    unsigned usage_count;
    unsigned long usage_min, usage_max;
    long logical_min, logical_max;
    long physical_min, physical_max;
    unsigned unit;
} hid_item_t;

typedef struct {
    unsigned report_id;
    hid_item_t *items;
    unsigned item_count, item_cap;
} hid_report_t;

typedef struct {
    unsigned usage_page;
    unsigned usage;
    hid_report_t *inputs, *outputs, *features;
    unsigned in_count, in_cap, out_count, out_cap, feat_count, feat_cap;
} hid_collection_t;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "out of memory\n"); exit(1); }
    return q;
}

static hid_report_t *collection_report(hid_collection_t *c, int kind, unsigned report_id)
{
    hid_report_t **arr;
    unsigned *cnt, *cap;
    if (kind == 0)      { arr = &c->inputs;   cnt = &c->in_count;   cap = &c->in_cap; }
    else if (kind == 1) { arr = &c->outputs;  cnt = &c->out_count;  cap = &c->out_cap; }
    else                { arr = &c->features; cnt = &c->feat_count; cap = &c->feat_cap; }

    for (unsigned i = 0; i < *cnt; i++)
        if ((*arr)[i].report_id == report_id)
            return &(*arr)[i];

    if (*cnt == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = xrealloc(*arr, *cap * sizeof(hid_report_t));
    }
    hid_report_t *r = &(*arr)[(*cnt)++];
    memset(r, 0, sizeof(*r));
    r->report_id = report_id;
    return r;
}

static void report_add_item(hid_report_t *r, hid_item_t *it)
{
    if (r->item_count == r->item_cap) {
        r->item_cap = r->item_cap ? r->item_cap * 2 : 8;
        r->items = xrealloc(r->items, r->item_cap * sizeof(hid_item_t));
    }
    r->items[r->item_count++] = *it;
}

/* Parses a raw report descriptor into a flat list of collections.
 * Returns the number of collections. Caller frees with
 * free_hid_collections(). */
static unsigned parse_report_descriptor(const unsigned char *d, int len,
                                        hid_collection_t **out_cols)
{
    hid_collection_t *cols = NULL;
    unsigned ncols = 0, ncap = 0;
    int stack[16];
    int sp = -1;

    unsigned g_usagePage = 0, g_reportSize = 0, g_reportId = 0, g_reportCount = 0;
    long g_logMin = 0, g_logMax = 0, g_physMin = 0, g_physMax = 0;
    unsigned g_unit = 0;

    unsigned long local_usages[MAX_USAGES];
    unsigned local_count = 0;
    int local_range = 0;
    unsigned long local_min = 0, local_max = 0;

    int i = 0;
    while (i < len) {
        unsigned char prefix = d[i++];
        unsigned bSize = prefix & 0x03;
        unsigned bTag  = (prefix >> 4) & 0x0F;
        unsigned bType = (prefix >> 2) & 0x03;
        unsigned size  = bSize == 3 ? 4 : bSize;
        if (i + (int)size > len) break;

        unsigned u = 0;
        for (unsigned k = 0; k < size; k++) u |= (unsigned)d[i + k] << (8 * k);
        long s = (long)((size == 4) ? (int)u
                      : size == 2 ? (short)u
                      : size == 1 ? (signed char)u : 0);
        i += (int)size;

        if (bType == 0) { /* Main items */
            if (bTag == 0x8 || bTag == 0x9 || bTag == 0xB) {
                int kind = bTag == 0x8 ? 0 : bTag == 0x9 ? 1 : 2;
                if (sp >= 0) {
                    hid_collection_t *c = &cols[stack[sp]];
                    hid_report_t *r = collection_report(c, kind, g_reportId);
                    hid_item_t it;
                    memset(&it, 0, sizeof(it));
                    it.report_size  = g_reportSize;
                    it.report_count = g_reportCount;
                    it.data_flags   = (unsigned char)u;
                    it.is_range     = local_range;
                    it.logical_min  = g_logMin;
                    it.logical_max  = g_logMax;
                    it.physical_min = g_physMin;
                    it.physical_max = g_physMax;
                    it.unit         = g_unit;
                    if (local_range) {
                        it.usage_min = local_min;
                        it.usage_max = local_max;
                    } else {
                        for (unsigned k = 0; k < local_count && k < MAX_USAGES; k++)
                            it.usages[it.usage_count++] = local_usages[k];
                    }
                    report_add_item(r, &it);
                }
                local_count = 0; local_range = 0; local_min = 0; local_max = 0;
            } else if (bTag == 0xA) { /* Collection */
                if (sp + 1 < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    if (ncols == ncap) {
                        ncap = ncap ? ncap * 2 : 4;
                        cols = xrealloc(cols, ncap * sizeof(hid_collection_t));
                    }
                    hid_collection_t *c = &cols[ncols];
                    memset(c, 0, sizeof(*c));
                    c->usage_page = g_usagePage;
                    c->usage      = local_count ? (unsigned)(local_usages[0] & 0xFFFF) : 0;
                    stack[++sp] = (int)ncols;
                    ncols++;
                }
                local_count = 0; local_range = 0; local_min = 0; local_max = 0;
            } else if (bTag == 0xC) { /* End Collection */
                if (sp >= 0) sp--;
            }
        } else if (bType == 1) { /* Global items */
            switch (bTag) {
                case 0x0: g_usagePage = u & 0xFFFF; break;
                case 0x1: g_logMin = s; break;
                case 0x2: g_logMax = s; break;
                case 0x3: g_physMin = s; break;
                case 0x4: g_physMax = s; break;
                case 0x5: break; /* unit exponent */
                case 0x6: g_unit = u; break;
                case 0x7: g_reportSize = u & 0xFF; break;
                case 0x8: g_reportId = u & 0xFF; break;
                case 0x9: g_reportCount = u & 0xFFFF; break;
            }
        } else if (bType == 2) { /* Local items */
            unsigned long combo = ((unsigned long)(g_usagePage & 0xFFFF) << 16) | (u & 0xFFFF);
            switch (bTag) {
                case 0x0:
                    if (local_count < MAX_USAGES) local_usages[local_count++] = combo;
                    break;
                case 0x1: local_range = 1; local_min = combo; break;
                case 0x2: local_range = 1; local_max = combo; break;
            }
        }
    }

    *out_cols = cols;
    return ncols;
}

static void free_hid_collections(hid_collection_t *cols, unsigned n)
{
    if (!cols) return;
    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = 0; j < cols[i].in_count; j++)
            free(cols[i].inputs[j].items);
        for (unsigned j = 0; j < cols[i].out_count; j++)
            free(cols[i].outputs[j].items);
        for (unsigned j = 0; j < cols[i].feat_count; j++)
            free(cols[i].features[j].items);
        free(cols[i].inputs);
        free(cols[i].outputs);
        free(cols[i].features);
    }
    free(cols);
}

/* ────────────────────────────────────────────────────────────
 * Printing
 * ──────────────────────────────────────────────────────────── */

static const char *usage_page_name(unsigned page)
{
    static const struct { unsigned p; const char *n; } pages[] = {
        { 0x01, "Generic Desktop" }, { 0x02, "Simulation" },
        { 0x03, "VR" },              { 0x04, "Sport" },
        { 0x05, "Game" },            { 0x06, "Generic Device" },
        { 0x07, "Keyboard" },        { 0x08, "LED" },
        { 0x09, "Button" },          { 0x0A, "Ordinal" },
        { 0x0B, "Telephony" },       { 0x0C, "Consumer" },
        { 0x0D, "Digitizer" },       { 0x0F, "Physical Interface" },
        { 0x80, "Monitor" },         { 0x84, "Power" },
        { 0x85, "Battery System" },  { 0x8C, "Barcode Scanner" },
        { 0x8D, "Scales" },          { 0x8E, "Camera Control" },
        { 0x8F, "Arcade" },          { 0xA0, "Gaming Device" },
    };
    for (size_t k = 0; k < sizeof(pages) / sizeof(pages[0]); k++)
        if (pages[k].p == page) return pages[k].n;
    if (page >= 0xFF00) return "Vendor-defined";
    return "Unknown";
}

static void print_usage(unsigned long combo)
{
    unsigned page = (unsigned)(combo >> 16);
    unsigned id   = (unsigned)(combo & 0xFFFF);
    printf("%04X:%04X (%s page 0x%04X usage 0x%04X)", page, id,
           usage_page_name(page), page, id);
}

static void print_item(const hid_item_t *it)
{
    unsigned bits = it->report_size * it->report_count;
    unsigned end  = bits ? bits - 1 : 0;

    printf("%u values * %u bits (bits 0 to %u)\n",
           it->report_count, it->report_size, end);
    printf("  %s,%s,%s\n",
           (it->data_flags & 0x01) ? "Cnst" : "Data",
           (it->data_flags & 0x02) ? "Ary" : "Var",
           (it->data_flags & 0x04) ? "Abs" : "Rel");

    if (it->is_range) {
        printf("  Usages: ");
        print_usage(it->usage_min);
        if (it->usage_min != it->usage_max) {
            printf(" to ");
            print_usage(it->usage_max);
        }
        printf("\n");
    } else if (it->usage_count) {
        printf("  Usages:\n");
        for (unsigned k = 0; k < it->usage_count; k++) {
            printf("    ");
            print_usage(it->usages[k]);
            printf("\n");
        }
    } else {
        printf("  Usages: none\n");
    }

    printf("  Logical bounds: %ld to %ld\n", it->logical_min, it->logical_max);
    if (it->physical_min != 0 || it->physical_max != 0)
        printf("  Physical bounds: %ld to %ld\n", it->physical_min, it->physical_max);
}

static void print_reports(const char *kind, const hid_report_t *reports, unsigned count)
{
    if (!count) return;
    printf("  %s reports:", kind);
    for (unsigned i = 0; i < count; i++)
        printf(" 0x%02X", reports[i].report_id);
    printf("\n");
    for (unsigned i = 0; i < count; i++) {
        printf("  %s report 0x%02X\n", kind, reports[i].report_id);
        for (unsigned j = 0; j < reports[i].item_count; j++) {
            printf("    ");
            print_item(&reports[i].items[j]);
        }
    }
}

/* Fetches and prints the full report descriptor structure. */
static void print_report_descriptor(hid_device *handle)
{
    unsigned char desc[MAX_DESC];
    int len = hid_get_report_descriptor(handle, desc, sizeof(desc));
    if (len <= 0) {
        printf("    Report descriptor: unavailable\n");
        return;
    }

    hid_collection_t *cols = NULL;
    unsigned ncols = parse_report_descriptor(desc, len, &cols);
    if (!ncols) {
        printf("    Report descriptor: %d bytes (no collections parsed)\n", len);
        free_hid_collections(cols, ncols);
        return;
    }

    printf("    Report descriptor: %d bytes\n", len);
    for (unsigned i = 0; i < ncols; i++) {
        const hid_collection_t *c = &cols[i];
        printf("    collection[%u]\n", i);
        printf("      Usage: ");
        print_usage(((unsigned long)c->usage_page << 16) | c->usage);
        printf("\n");
        print_reports("Input", c->inputs, c->in_count);
        print_reports("Output", c->outputs, c->out_count);
        print_reports("Feature", c->features, c->feat_count);
    }
    free_hid_collections(cols, ncols);
}

int main(void)
{
    struct hid_device_info *devs;
    struct hid_device_info *cur;
    int count = 0;

    setlocale(LC_ALL, "");

    if (hid_init()) {
        fprintf(stderr, "hid_init() failed\n");
        return 1;
    }

    devs = hid_enumerate(0x0, 0x0);

    if (!devs) {
        printf("No HID devices found.\n");
        hid_exit();
        return 0;
    }

    printf("========== HID devices ==========\n\n");

    for (cur = devs; cur; cur = cur->next) {

        printf("Device #%d\n", ++count);
        printf("  Path           : %s\n", cur->path);
        printf("  Vendor ID      : 0x%04X\n", cur->vendor_id);
        printf("  Product ID     : 0x%04X\n", cur->product_id);

        printf("  Manufacturer   : ");
        print_wstr(cur->manufacturer_string);
        printf("\n");

        printf("  Product        : ");
        print_wstr(cur->product_string);
        printf("\n");

        printf("  Serial         : ");
        print_wstr(cur->serial_number);
        printf("\n");

        printf("  Release number : 0x%04X\n", cur->release_number);
        printf("  Usage page     : 0x%04X\n", cur->usage_page);
        printf("  Usage          : 0x%04X\n", cur->usage);
        printf("  Interface      : %d\n", cur->interface_number);

#ifdef __linux__
        printf("  Bus type       : %d\n", cur->bus_type);
#endif

        /* Try to open device, print report descriptor and probe reports */
        hid_device *handle = hid_open_path(cur->path);
        if (handle) {
            print_report_descriptor(handle);
            probe_feature_reports(handle);
            probe_input_report(handle);
            hid_close(handle);
        } else {
            printf("  Reports: could not open device\n");
        }

        printf("\n");
    }

    printf("Total HID devices: %d\n", count);

    hid_free_enumeration(devs);
    hid_exit();
    return 0;
}
