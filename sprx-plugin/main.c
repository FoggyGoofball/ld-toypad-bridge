/**
 * main.c
 * LD-ToyPad Bridge SPRX plugin -- CellOS background thread entry point.
 *
 * Architecture (refactored):
 *   ldtoypad_start() is minimal -- spawns a background thread and returns
 *   SYS_PRX_START_OK immediately. This guarantees the bootloader is
 *   never blocked by network I/O or USB setup.
 *
 *   toypad_background_thread() owns all initialization and runs a
 *   continuous loop: poll USB endpoints, pump UDP traffic, sleep.
 *
 * Diagnostics:
 *   A boot log is written to /dev_hdd0/ldtoypad_boot.log via raw
 *   sysFs calls.  This works at the very first line of ldtoypad_start(),
 *   before any subsystem init, and survives crashes.  Retrieve it
 *   after boot via FTP.
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

/* ldoypad_start/ldtoypad_stop are tagged with __attribute__((visibility("default")))
 * to override -fvisibility=hidden in the Makefile. */

#define CONFIG_UDP_PORT          28472
#define CONFIG_MAIN_THREAD_PRIO  3072  /* ~= -0x400 in signed, low prio */
#define CONFIG_MAIN_THREAD_STACK 0x2000 /* 8 KB stack                  */
#define CONFIG_LOOP_SLEEP_USEC   10000  /* 10 ms between iterations    */

#define LDTP_ENABLE_FLAG_PATH "/dev_hdd0/plugins/ldtoypad.enable"
#define LDTP_BOOT_LOG_PATH    "/dev_hdd0/plugins/ldtoypad_boot.log"

/* ------------------------------------------------------------------------
 * PRX MODULE HEADER & EXPORT TABLE (Relocation-Free Assembly)
 * Forges the 32-bit PRX headers directly into the ELF to bypass
 * GCC R_PPC64_ADDR32 dynamic relocation errors.
 * ------------------------------------------------------------------------ */
__asm__(
    /* --- 1. Module Parameter Information --- */
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
    ".previous\n"

    /* --- 2. Module Metadata (CRITICAL FOR COBRA VALIDATION) --- */
    ".section .rodata.sceModuleInfo,\"a\"\n"
    ".align 2\n"
    ".short 0x0000\n"           /* Attributes */
    ".byte 1\n"                 /* Minor Version */
    ".byte 1\n"                 /* Major Version */
    ".ascii \"ldtoypad\"\n"     /* Module Name (8 bytes) */
    ".space 20\n"               /* Null padding to enforce 28-byte name limit */
    ".long 0x00000000\n"        /* TOC pointer (Resolved at kernel load time) */
    ".long __libentstart\n"
    ".long __libentend\n"
    ".long __libstubstart\n"
    ".long __libstubend\n"
    ".previous\n"

    /* --- 3. Module Entry Hook --- */
    ".section .lib.ent,\"a\"\n"
    ".align 2\n"
    ".long 0x01300000\n"        /* sys_prx_ent_info struct identifier (start) */
    ".long 0x00000000\n"
    ".long 0x00000000\n"
    ".long 0x00000000\n"
    ".long ldtoypad_start\n"    /* Exact match to C function signature */
    ".long 0x00000000\n"
    ".previous\n"

    /* --- 4. Module Exit Hook --- */
    ".section .lib.ent,\"a\"\n"
    ".align 2\n"
    ".long 0x01400000\n"        /* sys_prx_ent_info struct identifier (stop) */
    ".long 0x00000000\n"
    ".long 0x00000000\n"
    ".long 0x00000000\n"
    ".long ldtoypad_stop\n"     /* Exact match to C function signature */
    ".long 0x00000000\n"
    ".previous\n"
);

/* ---------------------------------------------------------------
 * Global run flag -- set to 0 to signal background thread exit
 * --------------------------------------------------------------- */
static volatile int g_running = 0;

/* ---------------------------------------------------------------
 * Background thread ID
 * --------------------------------------------------------------- */
static sys_ppu_thread_t g_thread_id = 0;

/* ---------------------------------------------------------------
 * Boot log -- write diagnostic trace to HDD via raw sysFs calls.
 *
 * Works at any point in ldtoypad_start(), before any subsystem init.
 * Survives crashes. Retrieve via FTP after boot.
 *
 * NOTE: sysFsOpen does NOT take a mode/permission argument.
 * Passing | 0666 corrupts the oflags (sets accmode=3, which is
 * invalid).  We use clean flags only.
 * --------------------------------------------------------------- */
static void boot_log_write(const char *msg)
{
    int fd;
    u64 written;

    if (sysFsOpen(LDTP_BOOT_LOG_PATH,
                  SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                  &fd, NULL, 0) == 0) {
        sysFsWrite(fd, msg, strlen(msg), &written);
        sysFsWrite(fd, "\n", 1, &written);
        sysFsClose(fd);
    }
}

static void boot_log_write_fmt(const char *fmt, s64 v)
{
    char buf[128];
    char num[32];
    int i = 0, j = 0;
    s64 n = v;

    /* Copy format string until %d */
    while (*fmt && *fmt != '%') {
        if (i < (int)sizeof(buf) - 2)
            buf[i++] = *fmt;
        fmt++;
    }

    /* Format the integer */
    if (n < 0) {
        if (i < (int)sizeof(buf) - 2) buf[i++] = '-';
        n = -n;
    }
    if (n == 0) {
        if (i < (int)sizeof(buf) - 2) buf[i++] = '0';
    } else {
        while (n > 0 && j < (int)sizeof(num) - 1) {
            num[j++] = '0' + (n % 10);
            n /= 10;
        }
        while (j > 0 && i < (int)sizeof(buf) - 2) {
            j--;
            buf[i++] = num[j];
        }
    }

    buf[i] = '\0';

    if (i > 0) {
        int fd;
        u64 written;
        if (sysFsOpen(LDTP_BOOT_LOG_PATH,
                      SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                      &fd, NULL, 0) == 0) {
            buf[i] = '\n';
            i++;
            sysFsWrite(fd, buf, i, &written);
            sysFsClose(fd);
        }
    }
}

/* ---------------------------------------------------------------
 * Enable gate -- strictly validated one-shot consumable token
 * --------------------------------------------------------------- */
static int check_enable_flag(void)
{
    sysFSStat stat;
    int unlink_res;

    if (sysFsStat(LDTP_ENABLE_FLAG_PATH, &stat) != 0) {
        boot_log_write("[BOOT] ENABLE FLAG NOT FOUND -- dormant");
        return 0;  /* no token -- stay dormant, boot is safe */
    }

    /* Strip restrictive attributes potentially set by FTP clients */
    sysFsChmod(LDTP_ENABLE_FLAG_PATH, 0666);

    /* Consume the token and capture the result */
    unlink_res = sysFsUnlink(LDTP_ENABLE_FLAG_PATH);
    
    /* ENFORCEMENT: Never arm if the token survives */
    if (unlink_res != 0) {
        boot_log_write_fmt("[BOOT] FATAL: sysFsUnlink failed (error %d) -- aborting arm sequence", unlink_res);
        return 0; /* FAILSAFE: Remain dormant to prevent infinite boot loops */
    }

    boot_log_write("[BOOT] enable token consumed -- plugin armed");
    DEBUG_PRINT("[LDTP] enable token consumed -- plugin armed for this boot\n");

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

    boot_log_write("[BOOT] background thread started");
    DEBUG_PRINT("[LDTP] background thread started (prio=%d)\n",
                CONFIG_MAIN_THREAD_PRIO);

    /* ---- Initialize subsystems ---- */

    debug_init();

    if (network_init(CONFIG_UDP_PORT) < 0) {
        DEBUG_ERROR("[LDTP] network_init failed -- thread exiting\n");
        boot_log_write("[BOOT] network_init FAILED -- thread exiting");
        g_running = 0;
        return;
    }
    boot_log_write("[BOOT] network_init OK");

    toypad_state_init();

    /* Extra LDD registration -- may fail if CFW doesn't support it.
     * That's OK: the plugin still works in network-only mode for
     * discovery beacons. */
    if (ldd_driver_init() < 0) {
        DEBUG_PRINT("[LDTP] LDD not available -- operating in network-only mode\n");
        boot_log_write("[BOOT] LDD init failed -- network-only mode");
    } else {
        boot_log_write("[BOOT] LDD init OK");
    }

    DEBUG_PRINT("[LDTP] Toy Pad bridge background service running on UDP %d\n",
                CONFIG_UDP_PORT);

    boot_log_write("[BOOT] main loop entering");

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

        /* Yield CPU -- use sysUsleep (LV2 syscall 141) */
        sysUsleep(CONFIG_LOOP_SLEEP_USEC);
    }

    /* ---- Shutdown ---- */

    ldd_driver_shutdown();
    toypad_state_deinit();
    network_shutdown();
    debug_shutdown();

    boot_log_write("[BOOT] background thread exiting");
    DEBUG_PRINT("[LDTP] background thread exiting\n");
}

/* ---------------------------------------------------------------
 * ldtoypad_start -- called by the kernel when loading the PRX
 *
 * MUST return quickly!  We spawn a background thread so the
 * bootloader is never blocked.
 * --------------------------------------------------------------- */
__attribute__((visibility("default"))) int ldtoypad_start(u64 args)
{
    int ret;

    (void)args;

    boot_log_write("[BOOT] ldtoypad_start() called");

    /* Gate: only activate when the enable flag is present */
    if (!check_enable_flag()) {
        /* Plugin remains dormant -- no threads, no memory */
        boot_log_write("[BOOT] dormant exit");
        return SYS_PRX_START_OK;
    }

    /* Prevent double-start */
    if (g_running) {
        boot_log_write("[BOOT] already running -- skipping");
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
        boot_log_write_fmt("[BOOT] sysThreadCreate FAILED ret=%d", ret);
        DEBUG_ERROR("[LDTP] sysThreadCreate failed: %d\n", ret);
        g_running = 0;
        return SYS_PRX_START_OK; /* still return OK -- don't hang boot */
    }

    boot_log_write("[BOOT] sysThreadCreate OK -- thread spawned");

    /* Return immediately -- bootloader continues */
    return SYS_PRX_START_OK;
}

/* ---------------------------------------------------------------
 * ldtoypad_stop -- called by the kernel when unloading the PRX
 *
 * Signals the background thread to exit and waits for it to join.
 * --------------------------------------------------------------- */
__attribute__((visibility("default"))) int ldtoypad_stop(void)
{
    u64 retval = 0;

    if (!g_running) {
        return SYS_PRX_STOP_OK;
    }

    boot_log_write("[BOOT] ldtoypad_stop() called -- shutting down");
    DEBUG_PRINT("[LDTP] stopping...\n");
    g_running = 0;

    if (g_thread_id != 0) {
        sysThreadJoin(g_thread_id, &retval);
        g_thread_id = 0;
    }

    boot_log_write("[BOOT] ldtoypad_stop() complete");
    DEBUG_PRINT("[LDTP] stopped\n");
    return SYS_PRX_STOP_OK;
}
