/**
 * main.c — LD-ToyPad Bridge SPRX — Thread-Based Init
 *
 * Injected into LEGO Dimensions via PS3MAPI.  A worker thread runs
 * the full init chain (boot log proved this works).  module_start
 * only creates the thread and returns immediately.
 *
 * Init chain in worker thread:
 *   debug_init() -> network_init() -> network_wait_ready()
 *   -> network_set_server() -> ldd_driver_init() -> toypad_state_init()
 *   -> main loop (recvfrom, server probes, 50ms yield)
 *
 * Sony SDK -mprx build.  No PSL1GHT.
 * -llv2_stub -lfs_stub -lnet_stub -lusbd_stub
 */
 
#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <cell/cell_fs.h>
#include <sys/socket.h>
#include <sys/sys_time.h>
#include <string.h>
#include <stddef.h>

#include "debug.h"
#include "network.h"
#include "ldd_driver.h"
#include "toypad_state.h"
#include "opd_hooks.h"
#include "usb_hooks.h"

SYS_MODULE_INFO(ldtoypad, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);

static void worker_thread(uint64_t arg);

static volatile int g_shutdown = 0;
static sys_ppu_thread_t g_worker_tid = SYS_PPU_THREAD_ID_INVALID;

/* g_init_progress — referenced by debug.c and usb_hooks.c */
volatile uint32_t g_init_progress = 0;

/** Write a line to /dev_hdd0/plugins/ldtoypad_boot.log */
int papertrail(const char *msg)
{
    int fd;
    uint64_t written;
    if (cellFsOpen("/dev_hdd0/plugins/ldtoypad_boot.log",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) != CELL_OK)
        return -1;
    cellFsWrite(fd, (void*)msg, (uint64_t)strlen(msg), &written);
    cellFsWrite(fd, "\n", 1, &written);
    cellFsClose(fd);
    return 0;
}

/* ================================================================
 * module_start — create worker thread, return immediately.
 *
 * Boot log evidence (2026-07-25) proves the worker thread runs
 * successfully: debug_init, network_init, ldd_driver_init,
 * toypad_state_init all executed in earlier tests.
 *
 * The issue was never thread creation — it was our diagnostic
 * mechanism (dcbst crashing on PRX pages) and ldd_driver_init
 * returning non-zero (USB module not loaded yet).
 * ================================================================ */
int module_start(size_t args, void *argp)
{
    (void)args; (void)argp;
    int ret;

    /* PS3MAPI may call module_start multiple times per injection.
     * Only create one worker thread to avoid port binding conflicts. */
    if (g_worker_tid != SYS_PPU_THREAD_ID_INVALID) {
        papertrail("module_start: worker already running, skipping duplicate");
        return 0;
    }

    papertrail("=== ldtoypad module_start (BUILD 2026-07-27-1100) ===");

    /* Create worker thread.  LDD registration happens as the VERY
     * FIRST action in the thread (before debug_init, before network)
     * to race the game's USB initialization. */
    ret = sys_ppu_thread_create(&g_worker_tid,
                                (void(*)(uint64_t))worker_thread,
                                0, 100, 64*1024,
                                SYS_PPU_THREAD_CREATE_JOINABLE,
                                "ldtoypad_worker");
    if (ret != CELL_OK) {
        papertrail("FATAL: thread create failed");
        return SYS_PRX_NO_RESIDENT;
    }

    papertrail("OK: worker thread created, returning resident");
    return 0;
}


int module_stop(void)
{
    papertrail("=== module_stop ===");
    g_shutdown = 1;

    if (g_worker_tid != SYS_PPU_THREAD_ID_INVALID) {
        uint64_t ev = 0;
        sys_ppu_thread_join(g_worker_tid, &ev);
        g_worker_tid = SYS_PPU_THREAD_ID_INVALID;
        papertrail("OK: worker joined");
    }

    opd_hooks_shutdown();
    usb_hook_shutdown();
    toypad_state_deinit();
    ldd_driver_shutdown();
    network_shutdown();
    debug_shutdown();

    papertrail("=== module_stop SUCCESS ===");
    return SYS_PRX_STOP_OK;
}

/* ================================================================
 * call_game_opd — Invoke a game function through its OPD from SPRX
 *
 * The game's probe/attach OPDs contain {code_addr, toc_addr, env}.
 * We save our SPRX TOC (r2), load the game's TOC, call via CTR,
 * then restore our TOC. Returns the game function's return value.
 * ================================================================ */
static int call_game_opd(uint32_t opd_addr, int dev_id)
{
    uint32_t *opd = (uint32_t*)opd_addr;
    uint32_t code = opd[0];
    uint32_t toc  = opd[1];
    int result;

    __asm__ volatile (
        "stwu   1, -0x80(1)\n\t"  /* Allocate stack frame */
        "mflr   0\n\t"            /* Save Link Register */
        "stw    0, 0x84(1)\n\t"   /* Store LR in caller's frame (+4) */

        "stw    2, 0x28(1)\n\t"   /* CRITICAL: Save SPRX TOC to stack! (NOT r11 — volatile!) */

        "mr     2, %1\n\t"        /* Load Game TOC into r2 */
        "mtctr  %2\n\t"           /* Move target code address to CTR */
        "mr     3, %3\n\t"        /* Move dev_id arg to r3 */

        "bctrl\n\t"               /* Branch and Link to Game Function */

        "lwz    2, 0x28(1)\n\t"   /* CRITICAL: Restore SPRX TOC from stack */

        "lwz    0, 0x84(1)\n\t"   /* Restore Link Register */
        "mtlr   0\n\t"            /* Move to LR */
        "addi   1, 1, 0x80\n\t"   /* Deallocate stack frame */
        "mr     %0, 3\n\t"        /* Capture return value from r3 */

        : "=r"(result)
        : "r"(toc), "r"(code), "r"(dev_id)

        /* Tell the compiler the game destroys basically everything */
        : "r0", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
          "ctr", "lr", "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "memory"
    );
    return result;
}

/* ================================================================
 * worker_thread — full init chain + UDP main loop.
 *
 * Boot log confirmed this runs.  All subsystems initialized here.
 * ================================================================ */
static void worker_thread(uint64_t arg)
{
    (void)arg;
    uint8_t seq = 0;
    int ret;

    papertrail("=== worker_thread started ===");

    /* ── 0. LDD FIRST — before ANY other syscall.
     * Must beat the game's USB init to claim the ToyPad.
     * No socket, no bind, no delay — straight to LDD registration. ── */
    ret = ldd_driver_init();
    if (ret == 0) {
        papertrail("LDD REGISTERED (first in thread) — racing game USB!");
    } else {
        papertrail("LDD deferred (USB not ready) — will retry after network");
    }

    /* ── 1. Debug ── */
    debug_init();
    papertrail("OK: debug_init()");

    /* ── 2. Network ── */
    if (network_init(28472) != 0) {
        papertrail("FATAL: network_init failed");
        debug_shutdown();
        sys_ppu_thread_exit(1);
        return;
    }
    papertrail("OK: network_init(28472)");

    network_wait_ready();
    papertrail("OK: network_wait_ready()");

    network_set_server(htonl(0xC0A80011), 28472);
    debug_set_remote(htonl(0xC0A80011), 28472);
    papertrail("OK: network_set_server(192.168.0.17:28472)");

    /* ── 3. LDD already registered in module_start — check status ── */
    if (g_ldd.registered) {
        papertrail("LDD: already registered (from module_start)");
    } else {
        /* Retry — USB may have become available since module_start */
        papertrail("LDD: retrying registration from worker thread...");
        ret = ldd_driver_init();
        if (ret == 0) {
            papertrail("OK: ldd_driver_init() — ToyPad LDD REGISTERED (retry)");
        } else {
            papertrail("NOTE: ldd_driver_init() returned non-zero — USB may not be ready");
        }
    }

    /* ── 4. ToyPad state ── */
    toypad_state_init();
    papertrail("OK: toypad_state_init()");

    /* ── 5. USB hooks (TROJAN HORSE strategy, 2026-07-26)
     *
     * The Trojan Horse approach uses the game's own event-driven USB
     * architecture: when a physical USB device is plugged in, the LV2
     * kernel wakes the game's probe() callback, which calls
     * cellUsbdGetDeviceDescriptor. Our hook lies about the VID/PID,
     * returning the ToyPad descriptor. The game's attach() callback
     * then fires, and our OpenPipe/InterruptTransfer hooks take over.
     *
     * We try the trampoline-based usb_hook_init() first (8 hooks:
     * OpenPipe, InterruptTransfer, ClosePipe, GetDeviceDescriptor,
     * ControlTransfer + 3 RegisterLdd capture hooks).
     * If it succeeds, we skip the older OPD-overwrite hooks entirely.
     * ── */
    ret = usb_hook_init();
    if (ret == 0) {
        papertrail("OK: usb_hook_init() returned success — check GOT count above");
        /* LDD ops harvested during init — trigger deferred to main loop */
    } else {
        papertrail("NOTE: usb_hook_init() failed — falling back to OPD hooks");
        /* ── 5b. Fallback: OPD hooks ── */
        ret = opd_hooks_init();
        if (ret == 0) {
            papertrail("OK: opd_hooks_init() — game USB calls INTERCEPTED!");
        } else {
            papertrail("NOTE: opd_hooks_init() — could not find hook target");
        }
    }

    /* ── 6. Main loop
     *
     * With the Trojan Horse (usb_hooks), all USB forwarding is handled
     * inline by the hook functions. The main loop only needs to:
     *   - Keep the thread alive (50ms sleep)
     *   - Send keepalive pings to the PC server
     *   - Handle server connection state
     *
     * With OPD hooks (fallback), the loop also handles HID forwarding
     * and hotplug firing for the older callback-stealing approach. ── */
    papertrail("=== Entering main loop ===");

    int use_opd_fallback = (ret != 0);  /* ret is from usb_hook_init above */
    int hotplug_fired = 0;
    int harvester_triggered = 0; /* Option 2: XMB rescan → RegisterLdd hook steals ops.
                                   Deferred trigger fires probe(0x99)/attach(0x99) via call_game_opd. */
    int loop_count = 0;

    while (!g_shutdown) {
        uint8_t buf[NET_PACKET_MAX_SIZE];
        loop_count++;

        /* 6a. Option 2 deferred trigger: wait 3s (60 ticks) for ops capture.
         * g_usb_hooks.ldd_ops_addr is set by my_cellUsbdRegisterLdd_hook
         * when user triggers XMB rescan. Must NOT be 0 (Harvester's false positive). */
        if (!harvester_triggered && loop_count > 60 && g_usb_hooks.ldd_ops_addr) {
            uint32_t *ops = (uint32_t*)g_usb_hooks.ldd_ops_addr;
            uint32_t probe_opd  = ops[1]; /* CellUsbdLddOps.probe */
            uint32_t attach_opd = ops[2]; /* CellUsbdLddOps.attach */
            if (probe_opd >= 0x00010000 && probe_opd < 0x02000000 &&
                attach_opd >= 0x00010000 && attach_opd < 0x02000000) {
                /* SAFETY: verify probe code starts with stwu (0x94xxxxxx) */
                uint32_t probe_code = ((uint32_t*)probe_opd)[0];
                uint32_t first_insn = ((uint32_t*)probe_code)[0];
                if ((first_insn >> 16) != 0x9421) {
                    papertrail("[TRIGGER] ABORT: probe code not stwu — bad OPD, skipping");
                    harvester_triggered = 1;
                } else {
                    papertrail("[TRIGGER] Calling probe(0x99)...");
                    int r = call_game_opd(probe_opd, 0x99);
                    {char msg[64];int mi=0;const char *s="[TRIGGER] probe returned ";while(*s)msg[mi++]=*s++;
                     if(r>=0){s="SUCCESS";while(*s)msg[mi++]=*s++;}else{s="FAIL";while(*s)msg[mi++]=*s++;}
                     msg[mi]=0;papertrail(msg);}
                    if (r >= 0) {
                        papertrail("[TRIGGER] Calling attach(0x99)...");
                        r = call_game_opd(attach_opd, 0x99);
                        {char msg[64];int mi=0;const char *s="[TRIGGER] attach returned ";while(*s)msg[mi++]=*s++;
                         if(r>=0){s="SUCCESS";while(*s)msg[mi++]=*s++;}else{s="FAIL";while(*s)msg[mi++]=*s++;}
                         msg[mi]=0;papertrail(msg);}
                    }
                    harvester_triggered = 1;
                }
            } else {
                papertrail("[TRIGGER] ldd_ops_addr set but probes not in EBOOT range");
            }
        }

        /* 6b. Fire hotplug once server connects (OPD fallback only). */
        if (use_opd_fallback && !hotplug_fired && opd_hooks_has_game_ops()) {
            papertrail("Attempting hotplug — firing game probe/attach...");
            int r = opd_hooks_fire_hotplug();
            if (r == 0) {
                hotplug_fired = 1;
                papertrail("HOTPLUG FIRED! Game should see ToyPad now!");
            }
        }

        /* 6b. Forward game→ToyPad commands to PC (OPD fallback only).
         * usb_hooks handles this inline in my_cellUsbdInterruptTransfer. */
        if (use_opd_fallback) {
            uint8_t out[64];
            int out_len = opd_hooks_get_out_data(out, sizeof(out));
            if (out_len > 0) network_send(out, out_len);
        }

        /* 6c. Receive PC→ToyPad responses (OPD fallback only).
         * usb_hooks handles this inline via network_send_poll/network_recv. */
        if (use_opd_fallback) {
            int n = network_recv(buf, sizeof(buf));
            if (n > 0 && opd_hooks_are_active()) {
                opd_hooks_push_in_data(buf, n);
            }
        }

        network_maybe_probe_server(seq++);
        network_send_keepalive();
        sys_timer_usleep(50000);
    }

    papertrail("=== worker_thread EXIT ===");
    sys_ppu_thread_exit(0);
}
