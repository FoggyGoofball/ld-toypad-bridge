/*
 * ds3-pair-daemon.c — Minimal FFS-based DualShock 3 emulator for one-time
 * Bluetooth pairing on Steam Deck (SteamOS / Arch Linux).
 *
 * Based on RosettaPad (github.com/ihasTaco/RosettaPad) FFS + ep0 handler.
 * Ported from Android JNI to plain Linux C.
 *
 * WHAT IT DOES:
 *   Presents as a DS3 over USB (VID 0x054C, PID 0x0268) via FunctionFS.
 *   Handles the PS3 ep0 authentication handshake (GET_REPORT 0xF2/0xF5/0xF7/0xF8/0x01,
 *   SET_REPORT 0xEF/0xF4/0xF5).  Captures the PS3's Bluetooth MAC address during
 *   SET_REPORT 0xF5.  Writes pairing data to ~/.config/ld-toypad/ds3-pairing.json.
 *   Exits once the handshake completes and pairing data is saved.
 *
 * BUILD (on Steam Deck):
 *   gcc -Wall -O2 -o ds3-pair-daemon ds3-pair-daemon.c
 *
 * RUN (as root, one-time setup):
 *   sudo ./ds3-pair-daemon
 *   # Then plug USB-C → PS3, wait for "Pairing complete" message.
 *   # Ctrl+C to exit.
 *
 * FFS gadget is created at /sys/kernel/config/usb_gadget/ds3-pair
 * Pairing data saved to ~/.config/ld-toypad/ds3-pairing.json
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <linux/usb/functionfs.h>

/* ── Configuration ──────────────────────────────────────────────── */
#define GADGET_DIR      "/sys/kernel/config/usb_gadget/ds3-pair"
#define FFS_MOUNT       "/dev/ffs-ds3"
#define PAIRING_JSON    "/home/deck/.config/ld-toypad/ds3-pairing.json"

#define USB_VID         0x054C
#define USB_PID         0x0268
#define USB_BCD_DEVICE  0x0100
#define USB_BCD_USB     0x0200  /* USB 2.0 — matches RosettaPad working reference */

/* ── DS3 Feature Report IDs ─────────────────────────────────────── */
#define DS3_REPORT_CAPS         0x01
#define DS3_REPORT_PAIRING      0xF5
#define DS3_REPORT_CALIB        0xF7
#define DS3_REPORT_STATUS       0xF8
#define DS3_REPORT_MAC          0xF2
#define DS3_REPORT_EF           0xEF
#define DS3_REPORT_LED          0xF4
#define DS3_FEATURE_REPORT_SIZE 64

/* ── Globals ────────────────────────────────────────────────────── */
static volatile int g_running = 1;
static int g_ep0_fd = -1;
static uint8_t g_ps3_mac[6] = {0};
static int g_ps3_mac_valid = 0;
static int g_usb_enabled = 0;
static int g_handshake_complete = 0;

/* ── DS3 Feature Report Templates ───────────────────────────────── */
/* Report 0x01 — Device capabilities */
static uint8_t report_01[64] = {
    0x00, 0x01, 0x04, 0x00, 0x08, 0x0C, 0x01, 0x02,
    0x18, 0x18, 0x18, 0x18, 0x09, 0x0A, 0x10, 0x11,
    0x12, 0x13, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x04,
    0x04, 0x04, 0x04, 0x00, 0x00, 0x04, 0x00, 0x01,
    0x02, 0x07, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Report 0xF2 — Controller Bluetooth MAC (bytes 4-9) */
/* Matches RosettaPad report_f2 byte-for-byte (was shifted by 2 bytes) */
static uint8_t report_f2[64] = {
    0xF2, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x50, 0x81, 0xD8, 0x01,
    0x8A, 0x13, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x04,
    0x00, 0x01, 0x02, 0x07, 0x00, 0x17, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Report 0xF5 — Host/PS3 Bluetooth MAC (bytes 2-7, filled at runtime) */
static uint8_t report_f5[64] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAE, 0x60, 0x00, 0x03, 0x50, 0x81, 0xD8, 0x01,
    0x8A, 0x13, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x04,
    0x04, 0x04, 0x04, 0x00, 0x00, 0x04, 0x00, 0x01,
    0x02, 0x07, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Report 0xF7 — Calibration */
static uint8_t report_f7[64] = {
    0x00, 0x02, 0xEC, 0x02, 0xD4, 0x01, 0x05, 0xFF,
    0x14, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Report 0xF8 — Status */
static uint8_t report_f8[64] = {
    0x00, 0x02, 0x00, 0x00, 0x08, 0x00, 0x03, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Report 0xEF — Configuration (echoed back on GET_REPORT after SET_REPORT) */
static uint8_t report_ef[64] = {
    0x00, 0xEF, 0x04, 0x00, 0x08, 0x00, 0x03, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* 49-byte idle input report (no buttons pressed, sticks centered) */
static uint8_t input_report[49] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x80, 0x80,   /* sticks centered */
    0x00, 0x00, 0x00, 0x00,   /* dpad pressure = 0 */
    0x00, 0x00, 0x00, 0x00,   /* reserved */
    0x00, 0x00,                 /* L2/R2 */
    0x00, 0x00,                 /* L1/R1 */
    0x00, 0x00, 0x00, 0x00,   /* face buttons */
    0x00, 0x00, 0x00,         /* reserved */
    0x02,                       /* plugged status */
    0x05,                       /* battery full */
    0x12,                       /* USB connection */
    0x00, 0x00, 0x00, 0x00,   /* reserved */
    0x33, 0x04,                 /* unknown fixed */
    0x77, 0x01,                 /* unknown fixed */
    0x00, 0x02,                 /* accel X */
    0x00, 0x02,                 /* accel Y */
    0x00, 0x02,                 /* accel Z */
    0x00, 0x02,                 /* gyro Z */
    0x02                        /* final byte */
};

/* ── FFS Descriptors ──────────────────────────────────────────── */
static const struct {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    struct {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio ep_in;
        struct usb_endpoint_descriptor_no_audio ep_out;
    } __attribute__((packed)) fs_descs, hs_descs;
} __attribute__((packed)) ffs_descriptors = {
    .header = {
        .magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
        .length = sizeof(ffs_descriptors),
        .flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
    },
    .fs_count = 3, .hs_count = 3,
    .fs_descs = {
        .intf = {
            .bLength = 9, .bDescriptorType = USB_DT_INTERFACE,
            .bInterfaceNumber = 0, .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = 0x03,   /* HID */
            .bInterfaceSubClass = 0,
            .bInterfaceProtocol = 0,
            .iInterface = 1,
        },
        .ep_in = {
            .bLength = 7, .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x81,   /* EP1 IN */
            .bmAttributes = 0x03,       /* Interrupt */
            .wMaxPacketSize = 64,
            .bInterval = 1,
        },
        .ep_out = {
            .bLength = 7, .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x02,   /* EP2 OUT */
            .bmAttributes = 0x03,       /* Interrupt */
            .wMaxPacketSize = 64,
            .bInterval = 1,
        },
    },
    /* HS descriptors identical to FS */
    .hs_descs = {
        .intf = {
            .bLength = 9, .bDescriptorType = USB_DT_INTERFACE,
            .bInterfaceNumber = 0, .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = 0x03,
            .bInterfaceSubClass = 0,
            .bInterfaceProtocol = 0,
            .iInterface = 1,
        },
        .ep_in = {
            .bLength = 7, .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x81,
            .bmAttributes = 0x03,
            .wMaxPacketSize = 64,
            .bInterval = 1,
        },
        .ep_out = {
            .bLength = 7, .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 0x02,
            .bmAttributes = 0x03,
            .wMaxPacketSize = 64,
            .bInterval = 1,
        },
    },
};

static const struct {
    struct usb_functionfs_strings_head header;
    struct {
        __le16 code;
        char str1[10];
    } __attribute__((packed)) lang0;
} __attribute__((packed)) ffs_strings = {
    .header = {
        .magic = FUNCTIONFS_STRINGS_MAGIC,
        .length = sizeof(ffs_strings),
        .str_count = 1,
        .lang_count = 1,
    },
    .lang0 = {
        .code = 0x0409,
        .str1 = "DS3 Input",
    },
};

/* ── Helpers ─────────────────────────────────────────────────── */
static void write_file(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, val, strlen(val)); close(fd); }
}

static void write_file_hex(const char *path, uint16_t val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%04x\n", val);
    write_file(path, buf);
}

static char *find_udc(void) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return NULL;
    struct dirent *e;
    static char udc[64];
    while ((e = readdir(d))) {
        if (e->d_name[0] != '.') {
            snprintf(udc, sizeof(udc), "%s", e->d_name);
            closedir(d);
            return udc;
        }
    }
    closedir(d);
    return NULL;
}

/* Get the Deck's Bluetooth MAC for report 0xF2/0xF5 */
static void get_deck_bt_mac(uint8_t mac[6]) {
    FILE *f = popen("hciconfig hci0 2>/dev/null | grep 'BD Address' | awk '{print $3}'", "r");
    if (!f) { memset(mac, 0, 6); return; }
    char line[32];
    if (fgets(line, sizeof(line), f)) {
        unsigned int a, b, c, d, e, f_val;
        if (sscanf(line, "%02X:%02X:%02X:%02X:%02X:%02X", &a, &b, &c, &d, &e, &f_val) == 6) {
            mac[0] = a; mac[1] = b; mac[2] = c; mac[3] = d; mac[4] = e; mac[5] = f_val;
        }
    }
    pclose(f);
}

/* Write pairing data to JSON */
static void save_pairing(const uint8_t *deck_mac, const uint8_t *ps3_mac) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", PAIRING_JSON);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); *slash = '/'; }

    FILE *f = fopen(PAIRING_JSON, "w");
    if (!f) { perror("save_pairing"); return; }
    fprintf(f, "{\n");
    fprintf(f, "  \"deck_bt_mac\": \"%02X:%02X:%02X:%02X:%02X:%02X\",\n",
            deck_mac[0], deck_mac[1], deck_mac[2], deck_mac[3], deck_mac[4], deck_mac[5]);
    fprintf(f, "  \"ps3_bt_mac\": \"%02X:%02X:%02X:%02X:%02X:%02X\"\n",
            ps3_mac[0], ps3_mac[1], ps3_mac[2], ps3_mac[3], ps3_mac[4], ps3_mac[5]);
    fprintf(f, "}\n");
    fclose(f);
    printf("Pairing data saved to %s\n", PAIRING_JSON);
}

/* ── Signal handler ──────────────────────────────────────────── */
static void sig_handler(int sig) { g_running = 0; }

/* ── ConfigFS gadget setup ───────────────────────────────────── */
static int setup_configfs(void) {
    system("modprobe libcomposite 2>/dev/null");
    system("modprobe usb_f_fs 2>/dev/null");

    /* Unbind ALL gadgets from UDC first (ToyPad g1 may be holding it) */
    printf("  Unbinding any existing gadgets from UDC...\n"); fflush(stdout);
    system("for d in /sys/kernel/config/usb_gadget/*/UDC; do [ -f \"$d\" ] && echo '' > \"$d\" 2>/dev/null; done; sleep 1");

    /* Tear down our specific gadget if it exists */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "if [ -f %s/UDC ]; then echo '' > %s/UDC 2>/dev/null; fi", GADGET_DIR, GADGET_DIR);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", GADGET_DIR);
    system(cmd);

    mkdir(GADGET_DIR, 0755);
    write_file_hex(GADGET_DIR "/idVendor", USB_VID);
    write_file_hex(GADGET_DIR "/idProduct", USB_PID);
    write_file_hex(GADGET_DIR "/bcdDevice", USB_BCD_DEVICE);
    write_file_hex(GADGET_DIR "/bcdUSB", USB_BCD_USB);

    mkdir(GADGET_DIR "/strings/0x409", 0755);
    write_file(GADGET_DIR "/strings/0x409/serialnumber", "123456");
    write_file(GADGET_DIR "/strings/0x409/manufacturer", "Sony");
    write_file(GADGET_DIR "/strings/0x409/product", "PLAYSTATION(R)3 Controller");

    mkdir(GADGET_DIR "/configs/c.1/strings/0x409", 0755);
    write_file(GADGET_DIR "/configs/c.1/strings/0x409/configuration", "DS3 Config");
    write_file(GADGET_DIR "/configs/c.1/MaxPower", "500");

    /* Create FFS function */
    mkdir(GADGET_DIR "/functions/ffs.usb0", 0755);
    snprintf(cmd, sizeof(cmd), "ln -sf %s/functions/ffs.usb0 %s/configs/c.1/ 2>/dev/null",
             GADGET_DIR, GADGET_DIR);
    system(cmd);

    /* Mount FunctionFS */
    mkdir(FFS_MOUNT, 0755);
    snprintf(cmd, sizeof(cmd), "mount -t functionfs usb0 %s 2>/dev/null", FFS_MOUNT);
    system(cmd);

    return 0;
}

static void teardown_configfs(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "if [ -f %s/UDC ]; then echo '' > %s/UDC 2>/dev/null; fi", GADGET_DIR, GADGET_DIR);
    system(cmd);
    usleep(500000);
    snprintf(cmd, sizeof(cmd), "umount %s 2>/dev/null", FFS_MOUNT);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", GADGET_DIR);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", FFS_MOUNT);
    system(cmd);
}

/* ── ep0 control thread ──────────────────────────────────────── */
static void *control_thread(void *arg) {
    (void)arg;
    struct usb_functionfs_event event;

    while (g_running) {
        ssize_t n = read(g_ep0_fd, &event, sizeof(event));
        if (n <= 0) { usleep(10000); continue; }

        switch (event.type) {
        case FUNCTIONFS_SETUP: {
            uint8_t  bRequest      = event.u.setup.bRequest;
            uint16_t wValue        = event.u.setup.wValue;
            uint16_t wLength       = event.u.setup.wLength;
            uint8_t  report_id     = wValue & 0xFF;

            printf("  ep0: bReq=0x%02X wVal=0x%04X wLen=%u\n",
                   bRequest, wValue, wLength);

            if (bRequest == 0x0A) {
                /* SET_IDLE — ACK */
                read(g_ep0_fd, NULL, 0);
            } else if (bRequest == 0x01) {
                /* GET_REPORT */
                const uint8_t *data = NULL;
                switch (report_id) {
                    case DS3_REPORT_CAPS:    data = report_01; break;
                    case DS3_REPORT_MAC:     data = report_f2; break;
                    case DS3_REPORT_PAIRING: data = report_f5; break;
                    case DS3_REPORT_CALIB:   data = report_f7; break;
                    case DS3_REPORT_STATUS:  data = report_f8; break;
                    case DS3_REPORT_EF:      data = report_ef; break;
                }
                if (data) {
                    size_t len = wLength < 64 ? wLength : 64;
                    write(g_ep0_fd, data, len);
                } else {
                    read(g_ep0_fd, NULL, 0); /* stall */
                }
            } else if (bRequest == 0x09) {
                /* SET_REPORT */
                uint8_t buf[64] = {0};
                if (wLength > 0) {
                    ssize_t r = read(g_ep0_fd, buf, wLength < 64 ? wLength : 64);
                    if (r > 0) {
                        if (report_id == DS3_REPORT_PAIRING && r >= 8) {
                            /* PS3 sends its BT MAC in SET_REPORT 0xF5 bytes 2-7 */
                            memcpy(g_ps3_mac, &buf[2], 6);
                            g_ps3_mac_valid = 1;
                            memcpy(&report_f5[2], &buf[2], 6);
                            printf("  Got PS3 BT MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                                   g_ps3_mac[0], g_ps3_mac[1], g_ps3_mac[2],
                                   g_ps3_mac[3], g_ps3_mac[4], g_ps3_mac[5]);
                            g_handshake_complete = 1;
                        } else if (report_id == DS3_REPORT_EF && r > 0) {
                            report_ef[0] = 0xEF;
                            memcpy(&report_ef[1], buf, r < 63 ? r : 63);
                        }
                    }
                }
                write(g_ep0_fd, NULL, 0); /* ACK */
            } else {
                read(g_ep0_fd, NULL, 0); /* stall unknown */
            }
            break;
        }
        case FUNCTIONFS_ENABLE:
            printf("  FFS ENABLE — PS3 enumerated the device\n");
            g_usb_enabled = 1;
            break;
        case FUNCTIONFS_DISABLE:
            printf("  FFS DISABLE — gadget unbound\n");
            break;
        case FUNCTIONFS_SUSPEND:
            break;
        }
    }
    return NULL;
}

/* ── Input report thread (keeps EP1 IN alive with idle reports) ─ */
static void *input_thread(void *arg) {
    (void)arg;
    int ep_in = -1;
    /* Wait for ep1 to appear */
    for (int i = 0; i < 50 && g_running; i++) {
        ep_in = open(FFS_MOUNT "/ep1", O_WRONLY);
        if (ep_in >= 0) break;
        usleep(100000);
    }
    if (ep_in < 0) { fprintf(stderr, "Could not open EP1 IN\n"); return NULL; }

    while (g_running && !g_handshake_complete) {
        write(ep_in, input_report, sizeof(input_report));
        usleep(8000); /* ~125Hz */
    }
    close(ep_in);
    return NULL;
}

/* ── Main ────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (geteuid() != 0) {
        fprintf(stderr, "Must run as root (sudo).\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Get Deck BT MAC */
    uint8_t deck_mac[6] = {0};
    get_deck_bt_mac(deck_mac);
    printf("Deck BT MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           deck_mac[0], deck_mac[1], deck_mac[2], deck_mac[3], deck_mac[4], deck_mac[5]);
    fflush(stdout);

    /* Set MAC in feature reports */
    memcpy(&report_f2[4], deck_mac, 6);
    memcpy(&report_f5[2], deck_mac, 6);

    /* Setup ConfigFS + FunctionFS */
    printf("Step 1/5: Setting up ConfigFS gadget...\n"); fflush(stdout);
    setup_configfs();
    printf("Step 2/5: Opening FFS ep0...\n"); fflush(stdout);

    /* Open FFS ep0 */
    g_ep0_fd = open(FFS_MOUNT "/ep0", O_RDWR);
    if (g_ep0_fd < 0) { perror("open ep0"); teardown_configfs(); return 1; }
    printf("Step 3/5: Writing descriptors...\n"); fflush(stdout);

    /* Write descriptors */
    write(g_ep0_fd, &ffs_descriptors, sizeof(ffs_descriptors));
    write(g_ep0_fd, &ffs_strings, sizeof(ffs_strings));

    /* Start control thread BEFORE binding UDC (RosettaPad order).
     * If we bind first, PS3 enumerates immediately and sends ep0 transfers
     * that get silently dropped because the control thread isn't listening yet.
     * This was the "silly mistake" — same class as menu script terminating early. */
    printf("Step 4/5: Starting control thread (before UDC bind)...\n"); fflush(stdout);
    pthread_t ctrl_tid, in_tid;
    pthread_create(&ctrl_tid, NULL, control_thread, NULL);
    pthread_create(&in_tid, NULL, input_thread, NULL);
    usleep(100000); /* Let threads initialize */

    /* Now bind to UDC — control thread is already listening */
    printf("Step 5/5: Binding to UDC...\n"); fflush(stdout);
    char *udc = find_udc();
    if (!udc) { fprintf(stderr, "No UDC found!\n"); teardown_configfs(); return 1; }
    printf("  UDC: %s\n", udc); fflush(stdout);

    char udc_path[256];
    snprintf(udc_path, sizeof(udc_path), "%s/UDC", GADGET_DIR);
    int udc_fd = open(udc_path, O_WRONLY);
    if (udc_fd < 0 || write(udc_fd, udc, strlen(udc)) < 0) {
        printf("ERROR: Failed to bind to UDC (may be in use by another gadget).\n");
        printf("  Run: for d in /sys/kernel/config/usb_gadget/*/UDC; do echo '' | sudo tee \"$d\"; done\n");
        if (udc_fd >= 0) close(udc_fd);
        teardown_configfs();
        return 1;
    }
    close(udc_fd);

    printf("\n========================================\n");
    printf("  DS3 Pairing Daemon v9.3.9\n");
    printf("  Descriptor size: %zu bytes\n", sizeof(ffs_descriptors));
    printf("  Plug USB-C cable into PS3 now.\n");
    printf("  Ctrl+C to exit.\n");
    printf("========================================\n\n");
    fflush(stdout);

    /* Wait for FFS_ENABLE (gadget bound to UDC and PS3 enumerated it) */
    printf("Waiting for PS3 to enumerate device...\n"); fflush(stdout);
    time_t start = time(NULL);
    while (g_running && !g_usb_enabled && (time(NULL) - start) < 30) {
        usleep(200000);
    }
    if (!g_usb_enabled) {
        printf("\nTIMEOUT: PS3 did not enumerate the device within 30 seconds.\n");
        printf("Check: USB cable (data, not charge-only), PS3 powered on, BIOS DRD mode.\n");
        g_running = 0;
        goto cleanup;
    }
    printf("PS3 enumerated device — waiting for handshake...\n");

    /* Wait for handshake */
    while (g_running && !g_handshake_complete && (time(NULL) - start) < 60) {
        usleep(200000);
    }
    if (!g_handshake_complete && g_running) {
        printf("\nTIMEOUT: Handshake did not complete within 60 seconds.\n");
    }

    if (g_handshake_complete && g_ps3_mac_valid) {
        printf("\n✓ Pairing complete!\n");
        printf("  Deck BT MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               deck_mac[0], deck_mac[1], deck_mac[2], deck_mac[3], deck_mac[4], deck_mac[5]);
        printf("  PS3 BT MAC:  %02X:%02X:%02X:%02X:%02X:%02X\n",
               g_ps3_mac[0], g_ps3_mac[1], g_ps3_mac[2], g_ps3_mac[3], g_ps3_mac[4], g_ps3_mac[5]);
        save_pairing(deck_mac, g_ps3_mac);
    }

cleanup:
    g_running = 0;
    pthread_join(in_tid, NULL);
    pthread_join(ctrl_tid, NULL);
    close(g_ep0_fd);
    teardown_configfs();

    printf("Done. Run bt-connect-ds3.sh to connect wirelessly.\n");
    return g_handshake_complete ? 0 : 1;
}
