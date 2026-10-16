/*
 * ugen300-loader: boot the ASUS UGen300 (Hailo-10H) USB accelerator without Linux.
 *
 * After every power cycle the UGen300 enumerates as a loader gadget and waits for the host
 * to upload its runtime root filesystem. On Linux that is done by Hailo's closed
 * `hailo_usb_loader rfs-upload`; this is a clean-room libusb implementation of the same
 * wire protocol (see docs/PROTOCOL.md), so the runtime can be started natively on macOS.
 *
 * Commands:
 *   probe        read-only: USB identity, loader protocol version, SKU and build string
 *   rfs-upload   upload the runtime image and wait for the runtime gadget to enumerate
 *
 * Firmware updates (`fw-update`) are deliberately not implemented: they write the stick's
 * flash and should stay with the vendor's tool.
 *
 * SPDX-License-Identifier: MIT
 */

#include <libusb.h>

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UGEN_VID 0x0b05
#define UGEN_PID 0x1d6f

#define PRODUCT_LOADER "UGen300 USB Loader"
#define PRODUCT_RUNTIME "UGen300 USB AI Accelerator (Hailo-10H)"
#define PRODUCT_FACTORY_LOADER "ASUSTek Computer, Inc. Hailo Composite USB Gadget"

#define DEFAULT_EXPECTED_BUILD "Hailo Linux build v1.10.10" /* loader shipped with HailoRT 5.3.0 */
#define IMAGE_NAME "core-image-hailo-hailo10-usb-dongle.ext4"

/* Loader gadget layout: configuration 1 = "RFS Loading Mode", interface 0, bulk OUT 0x01 */
#define CFG_RFS_LOADING 1
#define LOADER_INTERFACE 0
#define EP_BULK_OUT 0x01

/* Vendor requests (bmRequestType 0xC0 for IN, 0x40 for OUT; recipient = device) */
#define REQ_GET_BUILD_INFO 0x11   /* IN, 64: NUL-padded build string */
#define REQ_UPLOAD_START 0x12     /* OUT, wValue = size[15:0], wIndex = size[31:16] */
#define REQ_GET_SKU 0x14          /* IN, 64: NUL-terminated SKU string */
#define REQ_GET_STATUS 0x15       /* IN, wValue selects the status field below */
#define REQ_PROTOCOL_VERSION 0x16 /* IN, 4: u32 LE (OUT 0x16 with no data reboots the loader) */

#define STATUS_SERVER_READY 0 /* 1 byte, 1 once the receiver is up */
#define STATUS_RX_COUNTER 1   /* 4 bytes LE, bytes received on the bulk pipe */
#define STATUS_UPLOAD_READY 2 /* 1 byte, 1 when the image transfer may start */

#define SUPPORTED_PROTOCOL_VERSION 1
#define PING_SIZE 64
#define PING_BYTE 0xAA
#define CHUNK_SIZE 65536
#define XFER_TIMEOUT_MS 10000
#define POLL_INTERVAL_MS 100

static int g_verbose = 0;

static void logv(const char *fmt, ...)
{
    if (!g_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

enum stick_mode { MODE_ABSENT, MODE_LOADER, MODE_FACTORY_LOADER, MODE_RUNTIME, MODE_UNKNOWN };

static const char *mode_name(enum stick_mode m)
{
    switch (m) {
    case MODE_LOADER: return "loader (waiting for runtime upload)";
    case MODE_FACTORY_LOADER: return "factory loader (needs ASUS first-use firmware update)";
    case MODE_RUNTIME: return "runtime (HailoRT can use it)";
    case MODE_UNKNOWN: return "unknown";
    default: return "absent";
    }
}

static enum stick_mode mode_from_product(const char *product)
{
    if (strcmp(product, PRODUCT_LOADER) == 0) return MODE_LOADER;
    if (strcmp(product, PRODUCT_RUNTIME) == 0) return MODE_RUNTIME;
    if (strcmp(product, PRODUCT_FACTORY_LOADER) == 0) return MODE_FACTORY_LOADER;
    return MODE_UNKNOWN;
}

struct stick {
    libusb_device *dev; /* referenced; release with libusb_unref_device */
    char product[128];
    char serial[64];
    uint16_t bcd_device;
    uint8_t bus, address;
    enum stick_mode mode;
};

/* Find the first 0b05:1d6f device and read its strings. Returns false if none is present. */
static bool find_stick(libusb_context *ctx, struct stick *out)
{
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    bool found = false;

    memset(out, 0, sizeof(*out));
    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != UGEN_VID || desc.idProduct != UGEN_PID) continue;

        out->dev = libusb_ref_device(list[i]);
        out->bcd_device = desc.bcdDevice;
        out->bus = libusb_get_bus_number(list[i]);
        out->address = libusb_get_device_address(list[i]);
        out->mode = MODE_UNKNOWN;

        libusb_device_handle *h = NULL;
        if (libusb_open(list[i], &h) == 0) {
            if (desc.iProduct) {
                libusb_get_string_descriptor_ascii(h, desc.iProduct, (unsigned char *)out->product,
                                                   sizeof(out->product) - 1);
            }
            if (desc.iSerialNumber) {
                libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, (unsigned char *)out->serial,
                                                   sizeof(out->serial) - 1);
            }
            libusb_close(h);
            out->mode = mode_from_product(out->product);
        }
        found = true;
    }
    libusb_free_device_list(list, 1);
    return found;
}

static int ctrl_in(libusb_device_handle *h, uint8_t req, uint16_t wvalue, unsigned char *buf, uint16_t len)
{
    int r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                    req, wvalue, 0, buf, len, XFER_TIMEOUT_MS);
    logv("  ctrl IN  req=0x%02x wValue=%u -> %d\n", req, wvalue, r);
    return r;
}

static int ctrl_out(libusb_device_handle *h, uint8_t req, uint16_t wvalue, uint16_t windex)
{
    int r = libusb_control_transfer(h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                    req, wvalue, windex, NULL, 0, XFER_TIMEOUT_MS);
    logv("  ctrl OUT req=0x%02x wValue=0x%04x wIndex=0x%04x -> %d\n", req, wvalue, windex, r);
    return r;
}

static int bulk_out(libusb_device_handle *h, const unsigned char *data, int len)
{
    int sent = 0;
    int r = libusb_bulk_transfer(h, EP_BULK_OUT, (unsigned char *)data, len, &sent, XFER_TIMEOUT_MS);
    if (r == 0 && sent != len) {
        return LIBUSB_ERROR_IO;
    }
    return r;
}

static uint32_t le32(const unsigned char *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Select the RFS-loading configuration and claim its interface. macOS leaves a vendor-class
 * device unconfigured, so unlike Linux this must run before the first claim. */
static int configure_and_claim(libusb_device_handle *h)
{
    int r = libusb_set_configuration(h, CFG_RFS_LOADING);
    if (r != 0) {
        fprintf(stderr, "set configuration %d failed: %s\n", CFG_RFS_LOADING, libusb_error_name(r));
        return r;
    }
    r = libusb_claim_interface(h, LOADER_INTERFACE);
    if (r != 0) {
        fprintf(stderr, "claim interface %d failed: %s\n", LOADER_INTERFACE, libusb_error_name(r));
    }
    return r;
}

struct loader_info {
    uint32_t protocol_version;
    char sku[65];
    char build[65];
};

static int read_loader_info(libusb_device_handle *h, struct loader_info *info)
{
    unsigned char buf[64];
    int r;

    memset(info, 0, sizeof(*info));
    r = ctrl_in(h, REQ_PROTOCOL_VERSION, 0, buf, 4);
    if (r != 4) {
        fprintf(stderr, "failed to get protocol version: %s\n", r < 0 ? libusb_error_name(r) : "short read");
        return r < 0 ? r : LIBUSB_ERROR_IO;
    }
    info->protocol_version = le32(buf);

    memset(buf, 0, sizeof(buf));
    r = ctrl_in(h, REQ_GET_SKU, 0, buf, sizeof(buf));
    if (r < 0) {
        fprintf(stderr, "failed to get SKU ID: %s\n", libusb_error_name(r));
        return r;
    }
    memcpy(info->sku, buf, (size_t)r);

    memset(buf, 0, sizeof(buf));
    r = ctrl_in(h, REQ_GET_BUILD_INFO, 0, buf, sizeof(buf));
    if (r < 0) {
        fprintf(stderr, "failed to get build info: %s\n", libusb_error_name(r));
        return r;
    }
    memcpy(info->build, buf, (size_t)r);
    return 0;
}

static void print_stick(const struct stick *s)
{
    printf("Device:   usb bus %u addr %u  %04x:%04x  bcdDevice %x.%02x\n", s->bus, s->address, UGEN_VID,
           UGEN_PID, s->bcd_device >> 8, s->bcd_device & 0xff);
    printf("Product:  %s\n", s->product[0] ? s->product : "(unreadable)");
    printf("Serial:   %s\n", s->serial[0] ? s->serial : "(unreadable)");
    printf("Mode:     %s\n", mode_name(s->mode));
}

static int cmd_probe(libusb_context *ctx)
{
    struct stick s;
    if (!find_stick(ctx, &s)) {
        fprintf(stderr, "No UGen300 (%04x:%04x) found.\n", UGEN_VID, UGEN_PID);
        return 2;
    }
    print_stick(&s);

    int rc = 0;
    if (s.mode == MODE_LOADER || s.mode == MODE_FACTORY_LOADER) {
        libusb_device_handle *h = NULL;
        int r = libusb_open(s.dev, &h);
        if (r != 0) {
            fprintf(stderr, "open failed: %s\n", libusb_error_name(r));
            rc = 1;
        } else {
            struct loader_info info;
            if (configure_and_claim(h) == 0 && read_loader_info(h, &info) == 0) {
                printf("Protocol: %u\n", info.protocol_version);
                printf("SKU:      %s\n", info.sku);
                printf("Build:    %s\n", info.build);
            } else {
                rc = 1;
            }
            libusb_release_interface(h, LOADER_INTERFACE);
            libusb_close(h);
        }
    }
    libusb_unref_device(s.dev);
    return rc;
}

static unsigned char *read_image(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open image %s: %s\n", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > UINT32_MAX) {
        fprintf(stderr, "invalid image file: %s\n", path);
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    if (!buf) {
        fprintf(stderr, "out of memory reading %s\n", path);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        fprintf(stderr, "short read on %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (uint32_t)st.st_size;
    return buf;
}

static const char *default_image_path(void)
{
    static char path[1024];
    const char *env = getenv("UGEN300_IMAGE");
    if (env && *env) {
        return env;
    }
    const char *home = getenv("HOME");
    const char *candidates[] = {"%s/.local/share/ugen300/" IMAGE_NAME, "/opt/homebrew/share/ugen300/" IMAGE_NAME,
                                "/usr/local/share/ugen300/" IMAGE_NAME};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (i == 0) {
            if (!home) continue;
            snprintf(path, sizeof(path), candidates[0], home);
        } else {
            snprintf(path, sizeof(path), "%s", candidates[i]);
        }
        if (access(path, R_OK) == 0) {
            return path;
        }
    }
    return NULL;
}

/* Poll a one-byte status field until it reads 1. */
static int wait_status_flag(libusb_device_handle *h, uint16_t field, unsigned timeout_ms, const char *what)
{
    unsigned char b = 0;
    for (unsigned waited = 0;; waited += POLL_INTERVAL_MS) {
        int r = ctrl_in(h, REQ_GET_STATUS, field, &b, 1);
        if (r == 1 && b == 1) {
            return 0;
        }
        if (r < 0) {
            fprintf(stderr, "%s: status read failed: %s\n", what, libusb_error_name(r));
            return r;
        }
        if (waited >= timeout_ms) {
            fprintf(stderr, "%s: timed out\n", what);
            return LIBUSB_ERROR_TIMEOUT;
        }
        sleep_ms(POLL_INTERVAL_MS);
    }
}

static int read_rx_counter(libusb_device_handle *h, uint32_t *count)
{
    unsigned char buf[4];
    int r = ctrl_in(h, REQ_GET_STATUS, STATUS_RX_COUNTER, buf, 4);
    if (r != 4) {
        return r < 0 ? r : LIBUSB_ERROR_IO;
    }
    *count = le32(buf);
    return 0;
}

/* Two pings over the bulk pipe: the first read of the RX counter clears it, the second should
 * count exactly one ping. A mismatch is only a warning, as in the vendor loader. */
static int ping_bulk_pipe(libusb_device_handle *h)
{
    unsigned char ping[PING_SIZE];
    uint32_t count = 0;
    int r;

    memset(ping, PING_BYTE, sizeof(ping));
    r = bulk_out(h, ping, sizeof(ping));
    if (r == 0) r = read_rx_counter(h, &count);
    if (r != 0) {
        fprintf(stderr, "failed to clear RX counter: %s\n", libusb_error_name(r));
        return r;
    }
    sleep_ms(POLL_INTERVAL_MS);
    r = bulk_out(h, ping, sizeof(ping));
    if (r == 0) r = read_rx_counter(h, &count);
    if (r != 0) {
        fprintf(stderr, "ping failed: %s\n", libusb_error_name(r));
        return r;
    }
    if (count != PING_SIZE) {
        fprintf(stderr, "warning: ping counted %u bytes (expected %d)\n", count, PING_SIZE);
    }
    return 0;
}

static int upload_image(libusb_device_handle *h, const unsigned char *img, uint32_t size)
{
    int r = ctrl_out(h, REQ_UPLOAD_START, (uint16_t)(size & 0xffff), (uint16_t)(size >> 16));
    if (r < 0) {
        fprintf(stderr, "upload start failed: %s\n", libusb_error_name(r));
        return r;
    }
    r = wait_status_flag(h, STATUS_SERVER_READY, XFER_TIMEOUT_MS, "waiting for loader server");
    if (r != 0) return r;
    r = ping_bulk_pipe(h);
    if (r != 0) return r;
    r = wait_status_flag(h, STATUS_UPLOAD_READY, XFER_TIMEOUT_MS, "waiting for upload ready");
    if (r != 0) return r;

    double t0 = now_s();
    uint32_t off = 0;
    int last_pct = -1;
    while (off < size) {
        uint32_t n = size - off < CHUNK_SIZE ? size - off : CHUNK_SIZE;
        r = bulk_out(h, img + off, (int)n);
        if (r != 0) {
            fprintf(stderr, "\ndata transfer failed at offset %u: %s\n", off, libusb_error_name(r));
            return r;
        }
        off += n;
        int pct = (int)((uint64_t)off * 100 / size);
        if (pct != last_pct && isatty(STDOUT_FILENO)) {
            printf("\rUploading: %3d%%", pct);
            fflush(stdout);
            last_pct = pct;
        }
    }
    /* A zero-length packet tells the loader the image is complete */
    r = bulk_out(h, NULL, 0);
    if (r != 0) {
        fprintf(stderr, "\nfinal zero-length transfer failed: %s\n", libusb_error_name(r));
        return r;
    }
    double dt = now_s() - t0;
    printf("%sUploaded %u bytes in %.2f s (%.0f MB/s)\n", isatty(STDOUT_FILENO) ? "\r" : "", size, dt,
           dt > 0 ? size / dt / 1e6 : 0.0);
    return 0;
}

static bool wait_for_runtime(libusb_context *ctx, unsigned timeout_s)
{
    double deadline = now_s() + timeout_s;
    while (now_s() < deadline) {
        struct stick s;
        if (find_stick(ctx, &s)) {
            enum stick_mode m = s.mode;
            if (m == MODE_RUNTIME) {
                print_stick(&s);
            }
            libusb_unref_device(s.dev);
            if (m == MODE_RUNTIME) {
                return true;
            }
        }
        sleep_ms(500);
    }
    return false;
}

struct upload_opts {
    const char *image;
    const char *expected_build;
    bool force;
    unsigned wait_s;
};

static int cmd_rfs_upload(libusb_context *ctx, const struct upload_opts *o)
{
    struct stick s;
    if (!find_stick(ctx, &s)) {
        fprintf(stderr, "No UGen300 (%04x:%04x) found.\n", UGEN_VID, UGEN_PID);
        return 2;
    }
    print_stick(&s);
    if (s.mode == MODE_RUNTIME) {
        printf("Runtime is already running; nothing to do.\n");
        libusb_unref_device(s.dev);
        return 0;
    }
    if (s.mode == MODE_FACTORY_LOADER) {
        fprintf(stderr, "This stick still has its factory loader. Run ASUS's first-use firmware update on Linux first:\n"
                        "  https://github.com/Asus-UGen-Series/UGen300-USB-Firmware-Upgrade\n");
        libusb_unref_device(s.dev);
        return 3;
    }
    if (s.mode != MODE_LOADER && !o->force) {
        fprintf(stderr, "Unrecognised product string; refusing without --force.\n");
        libusb_unref_device(s.dev);
        return 3;
    }

    const char *path = o->image ? o->image : default_image_path();
    if (!path) {
        fprintf(stderr, "No runtime image found. Pass --image, set UGEN300_IMAGE, or run scripts/extract-image.sh.\n");
        libusb_unref_device(s.dev);
        return 2;
    }
    uint32_t size = 0;
    unsigned char *img = read_image(path, &size);
    if (!img) {
        libusb_unref_device(s.dev);
        return 2;
    }
    printf("Image:    %s (%u bytes)\n", path, size);

    libusb_device_handle *h = NULL;
    int rc = 1;
    int r = libusb_open(s.dev, &h);
    libusb_unref_device(s.dev);
    if (r != 0) {
        fprintf(stderr, "open failed: %s\n", libusb_error_name(r));
        free(img);
        return 1;
    }

    struct loader_info info;
    if (configure_and_claim(h) != 0 || read_loader_info(h, &info) != 0) {
        goto out;
    }
    printf("Loader:   protocol %u, SKU %s, %s\n", info.protocol_version, info.sku, info.build);
    if (info.protocol_version != SUPPORTED_PROTOCOL_VERSION) {
        fprintf(stderr, "Unsupported protocol version: %u\n", info.protocol_version);
        goto out;
    }
    if (strcmp(info.build, o->expected_build) != 0) {
        if (!o->force) {
            fprintf(stderr, "Unsupported build info: %s (expected %s). Use --expect-build or --force.\n", info.build,
                    o->expected_build);
            goto out;
        }
        fprintf(stderr, "warning: build %s != %s, continuing (--force)\n", info.build, o->expected_build);
    }

    /* The vendor loader re-selects the configuration before uploading; mirror it */
    libusb_release_interface(h, LOADER_INTERFACE);
    if (configure_and_claim(h) != 0) {
        goto out;
    }
    if (upload_image(h, img, size) != 0) {
        goto out;
    }
    libusb_release_interface(h, LOADER_INTERFACE);
    libusb_close(h);
    h = NULL;

    printf("Waiting up to %u s for the runtime to enumerate...\n", o->wait_s);
    if (wait_for_runtime(ctx, o->wait_s)) {
        printf("Runtime is up.\n");
        rc = 0;
    } else {
        fprintf(stderr, "Runtime did not appear within %u s.\n", o->wait_s);
    }

out:
    if (h) {
        libusb_release_interface(h, LOADER_INTERFACE);
        libusb_close(h);
    }
    free(img);
    return rc;
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: ugen300-loader [options] <probe|rfs-upload>\n"
            "\n"
            "  probe                 show the stick's mode; in loader mode also its protocol, SKU and build\n"
            "  rfs-upload            upload the runtime image and wait for the runtime to enumerate\n"
            "\n"
            "options:\n"
            "  -i, --image PATH      runtime image (default: $UGEN300_IMAGE, ~/.local/share/ugen300/%s)\n"
            "  -b, --expect-build S  loader build string to require (default: \"%s\")\n"
            "  -w, --wait SECONDS    how long to wait for the runtime (default: 30)\n"
            "  -f, --force           continue on an unexpected build or product string\n"
            "  -v, --verbose         log every USB request\n"
            "  -h, --help\n",
            IMAGE_NAME, DEFAULT_EXPECTED_BUILD);
}

int main(int argc, char **argv)
{
    struct upload_opts o = {NULL, DEFAULT_EXPECTED_BUILD, false, 30};
    static const struct option longopts[] = {
        {"image", required_argument, NULL, 'i'}, {"expect-build", required_argument, NULL, 'b'},
        {"wait", required_argument, NULL, 'w'},  {"force", no_argument, NULL, 'f'},
        {"verbose", no_argument, NULL, 'v'},     {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "i:b:w:fvh", longopts, NULL)) != -1) {
        switch (c) {
        case 'i': o.image = optarg; break;
        case 'b': o.expected_build = optarg; break;
        case 'w': o.wait_s = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'f': o.force = true; break;
        case 'v': g_verbose = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 64;
        }
    }
    if (optind != argc - 1) {
        usage(stderr);
        return 64;
    }
    const char *cmd = argv[optind];

    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r != 0) {
        fprintf(stderr, "failed to initialize libusb: %s\n", libusb_error_name(r));
        return 1;
    }

    int rc;
    if (strcmp(cmd, "probe") == 0) {
        rc = cmd_probe(ctx);
    } else if (strcmp(cmd, "rfs-upload") == 0) {
        rc = cmd_rfs_upload(ctx, &o);
    } else {
        usage(stderr);
        rc = 64;
    }
    libusb_exit(ctx);
    return rc;
}
