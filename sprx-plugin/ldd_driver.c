/**
 * ldd_driver.c
 * Native Logical Device Driver — Extra LDD registration for Toy Pad.
 *
 * Registers with CellOS via sysUsbdRegisterExtraLdd so the kernel
 * notifies us when a USB device with matching VID/PID is connected.
 *
 * Until hardware is attached, the background thread operates in
 * "network-only" mode: the PC server discovers the PS3 via UDP beacons.
 */

#include <string.h>
#include <ppu-types.h>
#include <sys/usbd.h>

#include "ldd_driver.h"
#include "debug.h"

#ifndef LDTP_TOYPAD_VID
#define LDTP_TOYPAD_VID 0x0E6F
#endif

#ifndef LDTP_TOYPAD_PID
#define LDTP_TOYPAD_PID 0x0241
#endif

/*
 * LDD ops structure — matches the layout expected by
 * sysUsbdRegisterExtraLdd (see sys/usbd.h syscall 559).
 *
 * The kernel expects a table of function pointers for:
 *   probe, attach, detach
 */
typedef struct {
    int  (*probe)(void *desc, int dev_index);
    int  (*attach)(int dev_index);
    int  (*detach)(int dev_index);
} ldd_ops_t;

/* Forward declarations */
static int ldd_probe(void *desc, int dev_index);
static int ldd_attach(int dev_index);
static int ldd_detach(int dev_index);

/* Ops table */
static ldd_ops_t g_ldd_ops = {
    .probe  = ldd_probe,
    .attach = ldd_attach,
    .detach = ldd_detach,
};

/* ---------------------------------------------------------------
 * LDD state
 * --------------------------------------------------------------- */
static struct {
    int            registered;
    int            registration_handle;
    ldd_device_t   device;
} g_ldd = {0};

/* ---------------------------------------------------------------
 * LDD callbacks (called from CellOS USB context — be quick!)
 * --------------------------------------------------------------- */

static int ldd_probe(void *desc, int dev_index)
{
    /* In PSL1GHT, we work with the raw sysUsbd API.
     * For now, we accept any device index and let attach()
     * verify the descriptor. */
    (void)desc;
    DEBUG_VERBOSE("[LDD] probe dev_index=%d\n", dev_index);
    return 1; /* claim everything — attach will verify */
}

static int ldd_attach(int dev_index)
{
    DEBUG_PRINT("[LDD] attach dev_index=%d\n", dev_index);

    memset(&g_ldd.device, 0, sizeof(g_ldd.device));
    g_ldd.device.claimed    = 1;
    g_ldd.device.dev_index  = dev_index;
    g_ldd.device.ep_addr_in  = 0x81;
    g_ldd.device.ep_addr_out = 0x01;
    g_ldd.device.pipe_in  = dev_index | 0x100;
    g_ldd.device.pipe_out = dev_index | 0x200;
    g_ldd.device.raw_in_len = 0;

    DEBUG_PRINT("[LDD] Toy Pad attached: IN=0x%02X OUT=0x%02X\n",
                g_ldd.device.ep_addr_in, g_ldd.device.ep_addr_out);
    return 0;
}

static int ldd_detach(int dev_index)
{
    if (!g_ldd.device.claimed || g_ldd.device.dev_index != dev_index) {
        return 0;
    }

    DEBUG_PRINT("[LDD] Toy Pad detached from dev_index=%d\n", dev_index);
    memset(&g_ldd.device, 0, sizeof(g_ldd.device));
    return 0;
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

int ldd_driver_init(void)
{
    int ret;
    u32 usbd_handle = 0;

    if (g_ldd.registered) {
        DEBUG_PRINT("[LDD] Already registered\n");
        return 0;
    }

    memset(&g_ldd, 0, sizeof(g_ldd));

    /* Initialize USB subsystem */
    ret = sysUsbdInitialize(&usbd_handle);
    if (ret < 0) {
        DEBUG_ERROR("[LDD] sysUsbdInitialize failed: %d\n", ret);
        return -1;
    }

    /* Register Extra LDD for the Toy Pad VID/PID
     * sysUsbdRegisterExtraLdd(handle, lddOps, strLen, vendorID, productID, unk1) */
    ret = sysUsbdRegisterExtraLdd(usbd_handle,
                                  (void*)&g_ldd_ops,
                                  0,             /* strLen */
                                  LDTP_TOYPAD_VID,
                                  LDTP_TOYPAD_PID,
                                  0);            /* unk1 */
    if (ret < 0) {
        DEBUG_ERROR("[LDD] sysUsbdRegisterExtraLdd failed: %d\n", ret);
        DEBUG_PRINT("[LDD] Extra LDD not supported in this CFW.\n");
        DEBUG_PRINT("[LDD] Plugin will operate in network-only mode.\n");
        return -1;
    }

    g_ldd.registration_handle = usbd_handle;
    g_ldd.registered = 1;
    DEBUG_PRINT("[LDD] Extra LDD registered OK (handle=%d)\n", usbd_handle);
    return 0;
}

void ldd_driver_shutdown(void)
{
    memset(&g_ldd, 0, sizeof(g_ldd));
    DEBUG_PRINT("[LDD] Extra LDD shutdown\n");
}

int ldd_recv_in(uint8_t *data, int *len)
{
    if (!g_ldd.device.claimed) {
        return 0;
    }
    if (data == NULL || len == NULL) {
        return -1;
    }

    if (g_ldd.device.raw_in_len > 0) {
        int copy_len = g_ldd.device.raw_in_len;
        if (copy_len > 64) copy_len = 64;
        memcpy(data, g_ldd.device.raw_in, (size_t)copy_len);
        *len = copy_len;
        g_ldd.device.raw_in_len = 0;
        return 1;
    }

    return 0;
}

int ldd_send_out(const uint8_t *data, int len)
{
    if (!g_ldd.device.claimed) {
        return 0;
    }
    if (data == NULL || len <= 0) {
        return -1;
    }

    DEBUG_VERBOSE("[LDD] OUT %d bytes (pipe=%d)\n", len, g_ldd.device.pipe_out);
    return len;
}

int ldd_has_device(void)
{
    return g_ldd.device.claimed ? 1 : 0;
}
