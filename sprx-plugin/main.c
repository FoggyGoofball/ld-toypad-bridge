/**
 * main.c
 * LD-ToyPad Bridge SPRX plugin -- CellOS background thread entry point.
 *
 * Architecture:
 *   module_start() evaluates the enable token. If missing, returns
 *   SYS_PRX_NO_RESIDENT immediately -- no background thread is spawned,
 *   preventing unconditional boot loops. If present, spawns a background
 *   thread and returns SYS_PRX_RESIDENT.
 *
 *   toypad_background_thread() owns all initialization and runs a
 *   continuous loop: poll USB endpoints, pump UDP traffic, sleep.
 *
 * PSL1GHT SDK API Conventions:
 *   Uses sysLv2FsOpen() for filesystem I/O (PSL1GHT LV2 wrapper),
 *   sys_ppu_thread_create() for thread management,
 *   and sysModuleLoad() for sysmodule dependency resolution.
 *   No proprietary CellOS SDK types (CellFsMode, etc.) are used.
 *
 * PRX metadata: provided by the stock PSL1GHT lv2-sprx.o (linked as a
 *   startup object), which supplies the correct .sys_proc_prx_param
 *   section with magic 0x1B434CEC, R_PPC64_ADDR32 relocation entries,
 *   and _start entry point. The SYS_MODULE_INFO/START/STOP macros
 *   (from <sys/prx.h>) generate the .rodata.sceModuleInfo section with
 *   correct NID exports for module_start/module_stop, replacing the
 *   need for a custom crt0.S.
 *
 * Diagnostics:
 *   A boot log is written to /dev_hdd0/plugins/ldtoypad_boot.log via raw
 *   sysFs calls.  This works at the very first line of module_start(),
 *   before any subsystem init, and survives crashes.
 */
 
#include <ppu-types.h>
#include <sys/prx.h>
#include <sys/systime.h>
#include <sys/ppu_thread.h>
#include <sys/sysmodule.h>
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

/* Write to /dev_hdd0/plugins/ which IS mounted at VSH plugin load time
 * (the kernel loads us from /dev_hdd0/plugins/ldtoypad.sprx itself).
 * This also keeps the boot log FTP-accessible via the same directory
 * as the plugin and enable token. */
#define LDTP_ENABLE_FLAG_PATH "/dev_hdd0/plugins/ldtoypad.enable"
#define LDTP_BOOT_LOG_PATH    "/dev_hdd0/plugins/ldtoypad_boot.log"

/* ---------------------------------------------------------------
 * PSL1GHT Native PRX Macros
 *
 * SYS_MODULE_INFO generates the .rodata.sceModuleInfo section with
 * correct PRX type (0x04) and module name.  SYS_MODULE_START/STOP
 * generate .lib.ent export entries for module_start/module_stop.
 *
 * The _start entry point and .sys_proc_prx_param metadata section
 * are provided by the stock PSL1GHT lv2-sprx.o startup object.
 * --------------------------------------------------------------- */
SYS_MODULE_INFO(ldtoypad, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);

/* ---------------------------------------------------------------
 * Global run flag -- set to 0 to signal background thread exit
 * --------------------------------------------------------------- */
static volatile int g_running = 0;

/* ---------------------------------------------------------------
 * Background thread ID
 * --------------------------------------------------------------- */
static sys_ppu_thread_t g_thread_id = 0;

/* ---------------------------------------------------------------
 * Boot log -- write diagnostic trace via raw sysLv2FsOpen calls.
 *
 * Uses the correct PSL1GHT LV2 filesystem API signature:
 *   sysLv2FsOpen(path, oflags, &fd, mode, NULL, 0)
 * where mode is a plain u32 value (no CellFsMode type).
 * --------------------------------------------------------------- */
static void boot_log_write(const char *msg)
{
    s32 fd;
    u64 written;
    u32 log_mode = 0666;

    if (sysLv2FsOpen(LDTP_BOOT_LOG_PATH,
                     SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                     &fd, log_mode, NULL, 0) == 0) {
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
        s32 fd;
        u64 written;
        u32 log_mode = 0666;
        if (sysLv2FsOpen(LDTP_BOOT_LOG_PATH,
                         SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                         &fd, log_mode, NULL, 0) == 0) {
            buf[i] = '\n';
            i++;
            sysFsWrite(fd, buf, i, &written);
            sysFsClose(fd);
        }
    }
}

/* Deferred unlink error buffer -- flushed to log after file descriptor
 * is successfully opened (Phase 1.1). */
static char g_unlink_error_buf[128] = "";

/* ---------------------------------------------------------------
 * Enable gate -- strictly validated one-shot consumable token
 * --------------------------------------------------------------- */
static int check_enable_flag(void)
{
    sysFSStat stat;
    int should_run = 0;
    int unlink_res;

    /* Isolate token presence evaluation */
    if (sysFsStat(LDTP_ENABLE_FLAG_PATH, &stat) == 0) {
        should_run = 1;
    } else {
        boot_log_write("[BOOT] ENABLE FLAG NOT FOUND -- dormant");
        return 0;
    }

    /* Strip restrictive attributes potentially set by FTP clients */
    sysFsChmod(LDTP_ENABLE_FLAG_PATH, 0666);

    /* Consume the token -- independent block, only if should_run */
    unlink_res = sysFsUnlink(LDTP_ENABLE_FLAG_PATH);

    /* Deport error handling to buffer; do NOT reset should_run.
     * Flushed to log file after fd is successfully opened. */
    if (unlink_res != 0) {
        /* ---- HARD ABORT: Token survived deletion ----
         * If we proceed and the plugin crashes during init (e.g. LDD
         * registration, socket bind), the token will be present on the
         * next boot and we'll be trapped in an infinite bootloop.
         * Reset should_run to 0 regardless of the stat result.
         * This restores the atomic gating model from v5 while keeping
         * the deferred error buffer for diagnostics. */
        should_run = 0;

        /* Format error into deferred buffer */
        {
            char num[32];
            int i = 0, j = 0;
            s64 n = (s64)unlink_res;
            const char *prefix = "[BOOT] sysFsUnlink failed (error ";

            while (*prefix && i < (int)sizeof(g_unlink_error_buf) - 2)
                g_unlink_error_buf[i++] = *prefix++;

            if (n < 0) {
                if (i < (int)sizeof(g_unlink_error_buf) - 2)
                    g_unlink_error_buf[i++] = '-';
                n = -n;
            }
            if (n == 0) {
                if (i < (int)sizeof(g_unlink_error_buf) - 2)
                    g_unlink_error_buf[i++] = '0';
            } else {
                while (n > 0 && j < (int)sizeof(num) - 1) {
                    num[j++] = '0' + (n % 10);
                    n /= 10;
                }
                while (j > 0 && i < (int)sizeof(num) - 1) {
                    j--;
                    g_unlink_error_buf[i++] = num[j];
                }
            }

            const char *suffix = ") -- continuing init";
            while (*suffix && i < (int)sizeof(g_unlink_error_buf) - 2)
                g_unlink_error_buf[i++] = *suffix++;

            g_unlink_error_buf[i] = '\0';
        }
    } else {
        boot_log_write("[BOOT] enable token consumed -- plugin armed");
        DEBUG_PRINT("[LDTP] enable token consumed -- plugin armed for this boot\n");
    }

    return should_run;
}

/* ---------------------------------------------------------------
 * The main background thread
 * --------------------------------------------------------------- */
static void toypad_background_thread(void *arg)
{
    uint8_t seq = 0;
    uint8_t udp_buf[NET_PACKET_MAX_SIZE];
    int usbd_ret;

    (void)arg;

    /* ---- CRT bypass: explicitly zero global structures ---- */
    memset(&g_debug, 0, sizeof(g_debug));
    memset(&g_net, 0, sizeof(g_net));
    memset(&g_ldd, 0, sizeof(g_ldd));
    memset(&g_toypad, 0, sizeof(g_toypad));

    /* ---- Task 2.2: Startup stabilization delay ---- */
    sys_ppu_thread_usleep(7000000); /* Strict 7-second hardware stabilization delay */

    /* ---- Task 2.3: USBD sysmodule dependency resolution (PSL1GHT API) ---- */
    usbd_ret = sysModuleLoad(SYSMODULE_USBD, 0);
    if (usbd_ret != 0) {
        boot_log_write("[BOOT] USBD sysmodule load FAILED -- thread exiting");
        sys_ppu_thread_exit(0);
        return; /* unreachable, but present for structural clarity */
    }

    /* Flush deferred unlink error buffer to log */
    if (g_unlink_error_buf[0] != '\0') {
        s32 fd;
        u64 written;
        u32 log_mode = 0666;
        if (sysLv2FsOpen(LDTP_BOOT_LOG_PATH,
                         SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                         &fd, log_mode, NULL, 0) == 0) {
            sysFsWrite(fd, g_unlink_error_buf, strlen(g_unlink_error_buf), &written);
            sysFsWrite(fd, "\n", 1, &written);
            sysFsClose(fd);
        }
        g_unlink_error_buf[0] = '\0';
    }

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
 * module_start -- called by the kernel when loading the PRX
 *
 * The _start entry point is provided by the stock PSL1GHT lv2-sprx.o
 * startup object (not a custom crt0.S).  lv2-sprx.o sets up the TOC
 * pointer (r2) and calls module_start().
 *
 * MUST return quickly!  We spawn a background thread so the
 * bootloader is never blocked.
 * --------------------------------------------------------------- */
__attribute__((visibility("default"))) int module_start(u64 args)
{
    int ret;

    (void)args;

    boot_log_write("[BOOT] module_start() called");

    /* Gate: only activate when the enable flag is present */
    if (!check_enable_flag()) {
        /* Plugin remains dormant -- no threads, no memory.
         * Return SYS_PRX_NO_RESIDENT so the kernel unloads us,
         * preventing unconditional boot loops. */
        boot_log_write("[BOOT] no enable token -- returning NO_RESIDENT");
        return SYS_PRX_NO_RESIDENT;
    }

    /* Prevent double-start */
    if (g_running) {
        boot_log_write("[BOOT] already running -- skipping");
        return SYS_PRX_START_OK;
    }

    g_running = 1;

    ret = sys_ppu_thread_create(&g_thread_id,
                                toypad_background_thread,
                                NULL,                         /* arg   */
                                CONFIG_MAIN_THREAD_PRIO,
                                (u64)CONFIG_MAIN_THREAD_STACK,
                                SYS_PPU_THREAD_CREATE_DETACHED,
                                "ldtoypad_bridge");
    if (ret < 0) {
        boot_log_write_fmt("[BOOT] sys_ppu_thread_create FAILED ret=%d", ret);
        DEBUG_ERROR("[LDTP] sys_ppu_thread_create failed: %d\n", ret);
        g_running = 0;
        return SYS_PRX_START_OK; /* still return OK -- don't hang boot */
    }

    boot_log_write("[BOOT] sys_ppu_thread_create OK -- thread spawned");

    /* Return SYS_PRX_RESIDENT -- PRX stays loaded */
    return SYS_PRX_RESIDENT;
}

/* ---------------------------------------------------------------
 * module_stop -- called by the kernel when unloading the PRX
 *
 * The dispatch is handled through the PRX NID export table generated
 * by the SYS_MODULE_STOP macro.  Signals the background thread to
 * exit via the g_running flag.
 *
 * NOTE: The background thread is created as DETACHED, so we do NOT
 * call sys_ppu_thread_join() here.  A detached thread's resources
 * are automatically reclaimed by the kernel when it exits.  Calling
 * join on a detached thread is undefined and will cause a kernel
 * panic.
 * --------------------------------------------------------------- */
__attribute__((visibility("default"))) int module_stop(void)
{
    if (!g_running) {
        return SYS_PRX_STOP_OK;
    }

    boot_log_write("[BOOT] module_stop() called -- shutting down");
    DEBUG_PRINT("[LDTP] stopping...\n");
    g_running = 0;

    /* No sys_ppu_thread_join() -- thread is DETACHED.
     * g_thread_id kept for diagnostic reference only. */

    boot_log_write("[BOOT] module_stop() complete");
    DEBUG_PRINT("[LDTP] stopped\n");
    return SYS_PRX_STOP_OK;
}
