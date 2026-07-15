/**
 * main.c
 * LD-ToyPad Bridge SPRX plugin — CellOS background thread entry point.
 *
 * Architecture (refactored):
 *   _start() is minimal — spawns a background thread and returns
 *   SYS_PRX_START_OK immediately. This guarantees the bootloader is
 *   never blocked by network I/O or USB setup.
 *
 *   toypad_background_thread() owns all initialization and runs a
 *   continuous loop: poll USB endpoints, pump UDP traffic, sleep.
 */

#include <ppu-types.h>
#include <sys/prx.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <lv2/sysfs.h>
#include <string.h>

#include "ldd_driver.h"
#include "network.h"
#include "toypad_state.h"
#include "debug.h"

#define CONFIG_UDP_PORT          28472
#define CONFIG_MAIN_THREAD_PRIO  3072  /* ~= -0x400 in signed, low prio */
#define CONFIG_MAIN_THREAD_STACK 0x2000 /* 8 KB stack                  */
#define CONFIG_LOOP_SLEEP_USEC   10000  /* 10 ms between iterations    */

#define LDTP_ENABLE_FLAG_PATH "/dev_hdd0/plugins/ldtoypad.enable"

SYS_PROCESS_PARAM_FIXED(1001, 0x4000)

__asm__(
    ".section .sys_proc_prx_param,\"a\"\n"
    ".align 3\n"
    ".long 0x00000028\n"
    ".long 0x1B434CEC\n"
    ".long 0x00000002\n"
    ".long 0x00000000\n"
    ".long __libentstart\n"
    ".long __libentend\n"
    ".long __libstubstart\n"
    ".long __libstubend\n"
    ".long 0x01010000\n"
    ".long 0x00000000\n"
    ".previous\n");

/* ---------------------------------------------------------------
 * Global run flag — set to 0 to signal background thread exit
 * --------------------------------------------------------------- */
static volatile int g_running = 0;

/* ---------------------------------------------------------------
 * Background thread ID
 * --------------------------------------------------------------- */
static sys_ppu_thread_t g_thread_id = 0;

/* ---------------------------------------------------------------
 * Enable gate — one-shot consumable token
 *
 * If /dev_hdd0/plugins/ldtoypad.enable exists, we delete it and
 * return 1 (arm the plugin).  On the next boot the token is gone,
 * so the plugin stays dormant.  This makes the "failsafe" safe:
 * any crash or hang during development is fixed by a simple power
 * cycle — no token, no thread.
 * --------------------------------------------------------------- */
static int check_enable_flag(void)
{
    sysFSStat stat;

    if (sysFsStat(LDTP_ENABLE_FLAG_PATH, &stat) != 0) {
        return 0;  /* no token — stay dormant, boot is safe */
    }

    /* Consume the token — delete it NOW so next boot is clean */
    sysFsUnlink(LDTP_ENABLE_FLAG_PATH);
    DEBUG_PRINT("[LDTP] enable token consumed — plugin armed for this boot\n");

    return 1;
}

/* ---------------------------------------------------------------
 * The main background thread
 * --------------------------------------------------------------- */
static void toypad_background_thread(void *arg)
{
    uint8_t seq = 0;
    uint8_t udp_buf[NET_PACKET_MAX_SIZE];

    (void)arg;

    DEBUG_PRINT("[LDTP] background thread started (prio=%d)\n",
                CONFIG_MAIN_THREAD_PRIO);

    /* ---- Initialize subsystems ---- */

    debug_init();

    if (network_init(CONFIG_UDP_PORT) < 0) {
        DEBUG_ERROR("[LDTP] network_init failed — thread exiting\n");
        g_running = 0;
        return;
    }

    toypad_state_init();

    /* Extra LDD registration — may fail if CFW doesn't support it.
     * That's OK: the plugin still works in network-only mode for
     * discovery beacons. */
    if (ldd_driver_init() < 0) {
        DEBUG_PRINT("[LDTP] LDD not available — operating in network-only mode\n");
    }

    DEBUG_PRINT("[LDTP] Toy Pad bridge background service running on UDP %d\n",
                CONFIG_UDP_PORT);

    /* ---- Main loop ---- */

    while (g_running) {

        /* 1. Try to receive from the PC server (non-blocking) */
        int recv_len = network_recv(udp_buf, (int)sizeof(udp_buf));
        if (recv_len > 0) {
            /* Forward received data to the USB OUT pipe if a device
             * is attached. Otherwise, we just acknowledge the server
             * heartbeat. */
            if (ldd_has_device()) {
                ldd_send_out(udp_buf, recv_len);
            }
        }

        /* 2. Try to read from the USB IN pipe (if device attached) */
        {
            uint8_t usb_data[64];
            int usb_len = 0;

            if (ldd_recv_in(usb_data, &usb_len) > 0 && usb_len > 0) {
                /* Forward USB data to PC server */
                network_send_data(1, seq, usb_data, usb_len);
            }
        }

        /* 3. If no server is known yet, send discovery probes */
        if (!network_has_server()) {
            network_maybe_probe_server(seq);
        }

        /* 4. Bump sequence and sleep */
        seq++;

        /* Yield CPU — use sysUsleep (LV2 syscall 141) */
        sysUsleep(CONFIG_LOOP_SLEEP_USEC);
    }

    /* ---- Shutdown ---- */

    ldd_driver_shutdown();
    toypad_state_deinit();
    network_shutdown();
    debug_shutdown();

    DEBUG_PRINT("[LDTP] background thread exiting\n");
}

/* ---------------------------------------------------------------
 * _start — called by the kernel when loading the PRX
 *
 * MUST return quickly!  We spawn a background thread so the
 * bootloader is never blocked.
 * --------------------------------------------------------------- */
int _start(u64 args)
{
    int ret;

    (void)args;

    /* Gate: only activate when the enable flag is present */
    if (!check_enable_flag()) {
        /* Plugin remains dormant — no threads, no memory */
        return SYS_PRX_START_OK;
    }

    /* Prevent double-start */
    if (g_running) {
        return SYS_PRX_START_OK;
    }

    g_running = 1;

    ret = sysThreadCreate(&g_thread_id,
                          toypad_background_thread,
                          NULL,                         /* arg   */
                          CONFIG_MAIN_THREAD_PRIO,
                          CONFIG_MAIN_THREAD_STACK,
                          THREAD_JOINABLE,
                          "ldtoypad_bridge");
    if (ret < 0) {
        DEBUG_ERROR("[LDTP] sysThreadCreate failed: %d\n", ret);
        g_running = 0;
        return SYS_PRX_START_OK; /* still return OK — don't hang boot */
    }

    /* Return immediately — bootloader continues */
    return SYS_PRX_START_OK;
}

/* ---------------------------------------------------------------
 * _stop — called by the kernel when unloading the PRX
 *
 * Signals the background thread to exit and waits for it to join.
 * --------------------------------------------------------------- */
int _stop(void)
{
    u64 retval = 0;

    if (!g_running) {
        return SYS_PRX_STOP_OK;
    }

    DEBUG_PRINT("[LDTP] stopping...\n");
    g_running = 0;

    if (g_thread_id != 0) {
        sysThreadJoin(g_thread_id, &retval);
        g_thread_id = 0;
    }

    DEBUG_PRINT("[LDTP] stopped\n");
    return SYS_PRX_STOP_OK;
}
