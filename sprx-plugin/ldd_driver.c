/**
 * ldd_driver.c — Extra LDD registration for Toy Pad (Sony SDK)
 *
 * Registers with CellOS via cellUsbdRegisterExtraLdd so the kernel
 * notifies us when a USB device with matching VID/PID is connected.
 *
 * Until hardware is attached, the background thread operates in
 * "network-only" mode: the PC server discovers the PS3 via UDP beacons.
 *
 * Sony SDK API (from <cell/usbd.h>):
 *   cellUsbdInit()                         — no arguments, returns int32_t
 *   cellUsbdRegisterExtraLdd(ops, vid, pid) — simplified signature
 *   CellUsbdLddOps { name, probe, attach, detach } — 3 callbacks + name
 */

#include <string.h>
#include <stdint.h>

#include <cell/usbd.h>
#include <cell/sysmodule.h>

#include "ldd_driver.h"
#include "debug.h"

/* papertrail — defined in main.c, writes to boot log */
extern int papertrail(const char *msg);

#ifndef LDTP_TOYPAD_VID
#define LDTP_TOYPAD_VID 0x0781   /* SanDisk VID */
#endif

#ifndef LDTP_TOYPAD_PID
#define LDTP_TOYPAD_PID 0x5581   /* SanDisk PID */
#endif

/* Forward declarations */
static int32_t ldd_probe(int32_t dev_id);
static int32_t ldd_attach(int32_t dev_id);
static int32_t ldd_detach(int32_t dev_id);

/*
 * Sony SDK CellUsbdLddOps structure.
 *   - name:     string identifier for the LDD
 *   - probe:    called when kernel finds a new device; return 0 to claim
 *   - attach:   called after probe succeeds; set up pipes etc.
 *   - detach:   called when device is removed; clean up
 *
 * Only 3 callbacks (no suspend/resume in the Sony SDK version).
 */
static CellUsbdLddOps g_ldd_ops = {
    .name   = "ldtoypad",
    .probe  = ldd_probe,
    .attach = ldd_attach,
    .detach = ldd_detach,
};

/* ---------------------------------------------------------------
 * LDD state
 * --------------------------------------------------------------- */
struct ldd_global_state g_ldd;

/* ---------------------------------------------------------------
 * LDD callbacks (called from CellOS USB context — be quick!)
 * --------------------------------------------------------------- */

static int32_t ldd_probe(int32_t dev_id)
{
    (void)dev_id;
    papertrail("LDD: probe called — claiming device!");
    DEBUG_PRINT("[LDD] *** PROBE dev_id=%d — CLAIMING ***\n", dev_id);
    return 0; /* claim — attach() will finalize */
}

static int32_t ldd_attach(int32_t dev_id)
{
    papertrail("LDD: *** ATTACH — ToyPad claimed! ***");
    DEBUG_PRINT("[LDD] *** ATTACH dev_id=%d — TOYPAD CLAIMED ***\n", dev_id);

    memset(&g_ldd.device, 0, sizeof(g_ldd.device));
    g_ldd.device.claimed    = 1;
    g_ldd.device.dev_index  = dev_id;
    g_ldd.device.ep_addr_in  = 0x81;
    g_ldd.device.ep_addr_out = 0x01;
    g_ldd.device.pipe_in  = dev_id | 0x100;
    g_ldd.device.pipe_out = dev_id | 0x200;
    g_ldd.device.raw_in_len = 0;

    papertrail("LDD: ToyPad endpoints IN=0x81 OUT=0x01");
    DEBUG_PRINT("[LDD] Toy Pad attached: IN=0x%02X OUT=0x%02X\n",
                g_ldd.device.ep_addr_in, g_ldd.device.ep_addr_out);
    return 0;
}

static int32_t ldd_detach(int32_t dev_id)
{
    if (!g_ldd.device.claimed || g_ldd.device.dev_index != dev_id) {
        return 0;
    }

    papertrail("LDD: ToyPad DETACHED");
    DEBUG_PRINT("[LDD] Toy Pad detached from dev_id=%d\n", dev_id);
    memset(&g_ldd.device, 0, sizeof(g_ldd.device));
    return 0;
}

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

int ldd_driver_init(void)
{
    int ret;

    if (g_ldd.registered) {
        DEBUG_PRINT("[LDD] Already registered\n");
        return 0;
    }

    memset(&g_ldd, 0, sizeof(g_ldd));

    /* Load USB system module first — the game may not have loaded it yet.
     * CELL_SYSMODULE_USBD must be active before cellUsbdInit can work.
     * This is idempotent — safe to call even if already loaded. */
    ret = cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);
    if (ret != CELL_OK) {
        DEBUG_ERROR("[LDD] cellSysmoduleLoadModule(USBD) failed: 0x%08X\n", ret);
        return -1;
    }
    DEBUG_PRINT("[LDD] cellSysmoduleLoadModule(USBD) OK\n");

    /* Initialize USB subsystem.  The game may have already called
     * cellUsbdInit(), returning 0x80110002 (already initialized).
     * Treat that as success and proceed to registration. */
    ret = cellUsbdInit();
    if (ret != CELL_OK && ret != 0x80110002) {
        DEBUG_ERROR("[LDD] cellUsbdInit failed: 0x%08X\n", ret);
        return -1;
    }
    if (ret == 0x80110002) {
        DEBUG_PRINT("[LDD] cellUsbdInit: USB already initialized, continuing\n");
    } else {
        DEBUG_PRINT("[LDD] cellUsbdInit OK\n");
    }


    /* Register Extra LDD for the Toy Pad VID/PID.
     * Accept any non-negative return — different CFW versions may
     * return different success codes (CELL_OK, CELL_USBD_PROBE_SUCCEEDED,
     * or custom values). */
    ret = cellUsbdRegisterExtraLdd(&g_ldd_ops,
                                   LDTP_TOYPAD_VID,
                                   LDTP_TOYPAD_PID);
    if (ret < 0) {
        DEBUG_ERROR("[LDD] cellUsbdRegisterExtraLdd failed: 0x%08X\n", ret);
        DEBUG_PRINT("[LDD] ToyPad LDD not registered (CFW may not support Extra LDD)\n");
        return -1;
    }

    g_ldd.registered = 1;
    DEBUG_PRINT("[LDD] Extra LDD registered OK\n");
    return 0;
}

void ldd_driver_shutdown(void)
{
    if (g_ldd.registered) {
        int ret = cellUsbdUnregisterExtraLdd(&g_ldd_ops);
        if (ret == CELL_USBD_PROBE_SUCCEEDED) {
            DEBUG_PRINT("[LDD] Extra LDD unregistered OK\n");
        } else {
            DEBUG_ERROR("[LDD] cellUsbdUnregisterExtraLdd failed: 0x%08X\n", ret);
        }
        g_ldd.registered = 0;
    }

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
