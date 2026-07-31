/*
 * hid-list.c
 *
 * Lists all USB HID devices using hidapi
 * and shows the reports supported by each device.
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

        /* Try to open device and probe reports */
        hid_device *handle = hid_open_path(cur->path);
        if (handle) {
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
