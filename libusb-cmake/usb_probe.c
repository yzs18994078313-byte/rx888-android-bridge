#include <libusb.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    libusb_context *ctx = NULL;
    int result = libusb_init(&ctx);
    if (result != LIBUSB_SUCCESS) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(result));
        return 1;
    }
    printf("uid=%u libusb=%u.%u.%u\n", (unsigned)getuid(),
        libusb_get_version()->major, libusb_get_version()->minor,
        libusb_get_version()->micro);
    libusb_device **devices = NULL;
    ssize_t count = libusb_get_device_list(ctx, &devices);
    if (count < 0) {
        fprintf(stderr, "enumeration: %s\n", libusb_error_name((int)count));
        libusb_exit(ctx);
        return 2;
    }
    int found = 0, opened = 0;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devices[i], &desc) != 0) continue;
        printf("%03u:%03u %04x:%04x speed_enum=%d",
            libusb_get_bus_number(devices[i]), libusb_get_device_address(devices[i]),
            desc.idVendor, desc.idProduct, libusb_get_device_speed(devices[i]));
        if (desc.idVendor == 0x2500) {
            ++found;
            libusb_device_handle *handle = NULL;
            result = libusb_open(devices[i], &handle);
            printf(" Ettus_open=%s", libusb_error_name(result));
            if (result == 0) { ++opened; libusb_close(handle); }
        }
        putchar('\n');
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(ctx);
    printf("Ettus devices=%d opened=%d (no firmware written, no streams started)\n", found, opened);
    return opened ? 0 : 3;
}
