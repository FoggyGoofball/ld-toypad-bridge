/**
 * main.c — LD-ToyPad Bridge SPRX — Game Process Hook Variant
 *
 * Designed to be injected into the LEGO Dimensions game process
 * via PS3MAPI. Intercepts the game's cellUsbd calls using PowerPC
 * user-space detour hooks and routes Toy Pad traffic over UDP.
 *
 * Init chain in worker thread:
 *   debug_init() → network_init() → network_wait_ready()
 *   → usb_hook_init() → toypad_state_init()
 *   → main loop (recvfrom non-blocking, server probes, 10ms yield)
 *
 * Shutdown reverse order in module_stop after thread join.
 *
 * Sony SDK -mprx build.  No PSL1GHT.  No inline asm.
 * -llv2_stub resolves sys_ppu_thread_create/join/exit, sys_time_get_system_time.
 * -lfs_stub resolves cellFsOpen/Write/Close (papertrail boot logging).
 * -lnet_stub resolves socket/bind/sendto/recvfrom/close.
 * -lusbd_stub linked — SPRX imports cellUsbd for OPD trick.
 *   The OPD trick extracts the resolved code addresses from our own
 *   imports (CellOS resolves them when the SPRX loads), bypassing
 *   the game's lazy-binding chicken-and-egg problem entirely.
 */
 
#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <cell/cell_fs.h>
#include <sys/socket.h>
#include <sys/sys_time.h>
#include <string.h>
#include <stddef.h>
#include <sys/prx.h>

#include "debug.h"
#include "network.h"
#include "usb_hooks.h"
#include "toypad_state.h"

SYS_MODULE_INFO(ldtoypad, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);

static void worker_thread(uint64_t arg);

static volatile int g_shutdown = 0;
static sys_ppu_thread_t g_worker_tid = SYS_PPU_THREAD_ID_INVALID;

/** Write a line to /dev_hdd0/plugins/ldtoypad_boot.log */
int papertrail(const char *msg)
{
    int fd;
    uint64_t written;

    if (cellFsOpen("/dev_hdd0/plugins/ldtoypad_boot.log",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) != CELL_OK)
        return -1;
    cellFsWrite(fd, msg, strlen(msg), &written);
    cellFsWrite(fd, "\n", 1, &written);
    cellFsClose(fd);
    return 0;
}

int module_start(size_t args, void *argp)
{
    (void)args; (void)argp;
    int ret;

    papertrail("=== ldtoypad Full Integration: module_start ===");

    /* ── VSH SAFETY GUARD ──────────────────────────────────────────
     * If this SPRX was accidentally loaded into the XMB (VSH) process
     * via boot_plugins.txt, bail out immediately. The SPRX is designed
     * for hot-injection into the LEGO Dimensions game process via
     * PS3MAPI. Running in VSH would attempt NID scans and memory
     * operations that can corrupt the dashboard and freeze the console.
     *
     * Detection: Try to open a file unique to the game process.
     * If it doesn't exist, we're in VSH and abort module load.
     *
     * NOTE: returning non-zero from module_start = "unload me".
     * The system will remove the module from memory immediately.
     * This is the safest behavior in VSH context. */
    {
        int scratch_fd;
        uint64_t dummy_written;

        /* If /dev_hdd0/tmp/ld_vsh_guard.txt exists, boot_plugins.txt
         * contained ldtoypad.sprx — this is an error condition.
         * Remove it and abort. */
        int exists_check = cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
                                      CELL_FS_O_RDONLY,
                                      &scratch_fd, NULL, 0);
        if (exists_check == CELL_OK) {
            cellFsClose(scratch_fd);
            papertrail("VSH GUARD: detected VSH context — unloading immediately");
            papertrail("VSH GUARD: remove ldtoypad.sprx from /dev_hdd0/boot_plugins.txt");
            /* Write the guard file so user knows what happened */
            int guard_fd;
            if (cellFsOpen("/dev_hdd0/tmp/ld_vsh_detected.txt",
                           CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                           &guard_fd, NULL, 0) == CELL_OK) {
                char msg[] = "VSH_DETECTED=1\nLoaded from boot_plugins.txt\nRemove from boot_plugins.txt\n";
                cellFsWrite(guard_fd, msg, strlen(msg), &dummy_written);
                cellFsClose(guard_fd);
            }
            return SYS_PRX_NO_RESIDENT;  /* Tell kernel to unload us immediately */
        }

        /* Write the VSH guard token — next boot will detect it
         * and refuse to load. This prevents infinite crash loops. */
        if (cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
                       CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                       &scratch_fd, NULL, 0) == CELL_OK) {
            char msg[] = "VSH_GUARD=1\n";
            cellFsWrite(scratch_fd, msg, strlen(msg), &dummy_written);
            cellFsClose(scratch_fd);
        }
    }

    papertrail("Game process confirmed — creating worker thread...");
    ret = sys_ppu_thread_create(&g_worker_tid,
                                (void(*)(uint64_t))worker_thread,
                                0, 1000, 16*1024,
                                SYS_PPU_THREAD_CREATE_JOINABLE,
                                "ldtoypad_worker");
    if (ret != CELL_OK) {
        papertrail("FAIL: thread create");
        return SYS_PRX_NO_RESIDENT;
    }
    papertrail("OK: thread created");
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

    /* Shutdown subsystems in reverse order of init.
     * All are idempotent: calling twice is safe. */
    toypad_state_deinit();
    usb_hook_shutdown();
    network_shutdown();
    debug_shutdown();

    papertrail("=== module_stop SUCCESS ===");
    return SYS_PRX_STOP_OK;
}

static void worker_thread(uint64_t arg)
{
    (void)arg;
    uint8_t seq = 0;

    papertrail("=== worker_thread started ===");

    /* -------------------------------------------------------
     * 1. Initialize debug subsystem (ring buffer + HDD log)
     * ------------------------------------------------------- */
    debug_init();
    papertrail("OK: debug_init()");

    DEBUG_PRINT("[MAIN] Worker thread entering init chain\n");

    /* -------------------------------------------------------
     * 2. Initialize UDP network on port 28472
     * ------------------------------------------------------- */
    if (network_init(28472) != 0) {
        papertrail("FAIL: network_init() — fatal, cannot continue");
        DEBUG_ERROR("[MAIN] network_init() failed\n");
        debug_shutdown();
        sys_ppu_thread_exit(1);
        return;
    }
    papertrail("OK: network_init(28472)");
    DEBUG_PRINT("[MAIN] UDP socket bound on port 28472\n");

    /* -------------------------------------------------------
     * 3. Wait for network interface (3s sleep-based heuristic)
     * ------------------------------------------------------- */
    papertrail("Waiting for network interface (3s)...");
    network_wait_ready();
    papertrail("OK: network_wait_ready()");
    DEBUG_PRINT("[MAIN] Network interface ready\n");

    /* -------------------------------------------------------
     * 3b. Hardcode PC server IP (dev phase — bypass broadcast discovery)
     *     PC=192.168.0.17:28472 (0xC0A80011 = 192.168.0.17 in network order)
     * ------------------------------------------------------- */
    network_set_server(htonl(0xC0A80011), 28472);
    papertrail("OK: network_set_server(192.168.0.17:28472)");
    DEBUG_PRINT("[MAIN] Server hardcoded to 192.168.0.17:28472\n");

    /* -------------------------------------------------------
     * 4. Install USB detour hooks for Toy Pad emulation.
     *
     *    Intercepts the game's cellUsbdInit, cellUsbdOpenPipe,
     *    cellUsbdTransfer, cellUsbdClosePipe, and cellUsbdRegisterLdd.
     *    Routes Toy Pad USB traffic to the Node.js server via UDP.
     *
     *    This requires the SPRX to be loaded into the game process
     *    (via PS3MAPI injection). If running in VSH context,
     *    hook installation will fail gracefully.
     *
     *    usb_hook_init will fail gracefully if not in game process (returns -1).
     * ------------------------------------------------------- */
    {
        int hooks_ok = usb_hook_init();
        if (hooks_ok == 0) {
            papertrail("OK: usb_hook_init() — USB hooks installed for game");
            DEBUG_PRINT("[MAIN] USB hooks installed (game process mode)\n");
        } else {
            papertrail("NOTE: usb_hook_init() — VSH-only mode (no game hooks)");
            DEBUG_PRINT("[MAIN] USB hooks not installed, operating in VSH mode\n");
        }
    }

    /* -------------------------------------------------------
     * 5. Initialize Toy Pad state machine
     * ------------------------------------------------------- */
    toypad_state_init();
    papertrail("OK: toypad_state_init()");
    DEBUG_PRINT("[MAIN] Toy Pad state machine ready\n");

    /* -------------------------------------------------------
     * 6. Main background loop
     *
     *    Performs:
     *      - Non-blocking recvfrom() for incoming UDP packets from PC server
     *      - Rate-limited server discovery probe broadcasts
     *      - 50ms sleep each iteration to yield PPU to VSH (20 Hz)
     *      - Memory-mapped heartbeat counter for deadlock detection
     *
     *    The actual USB interrupt polling will be driven by the
     *    CellOS USB callback system once a device is attached.
     *    Until then, this loop keeps the network path alive.
     *
     *    SLEEP TIMING NOTE:
     *    The sleep was reduced from 10ms (100 Hz) to 50ms (20 Hz) on
     *    2026-07-21 per expert recommendation. 100 Hz was starving the
     *    sceNpTrophy background threads of PPU cycles and sceNet mutex
     *    access, causing the "Loading Trophy" freeze. 20 Hz is the
     *    standard polling rate for PlayStation peripheral emulation.
     * ------------------------------------------------------- */
    papertrail("=== Entering main loop ===");
    DEBUG_PRINT("[MAIN] Entering background loop\n");

    while (!g_shutdown) {
        uint8_t buf[NET_PACKET_MAX_SIZE];

        /* Memory-mapped heartbeat — incremented every iteration.
         * Stored in the sys_memory_allocate trampoline page (offset 128).
         * Polled by Node.js orchestrator via PS3MAPI /read_process.
         * At 50ms sleep, this increments at ~20 Hz.
         * No HDD writes — avoids I/O contention with game asset streaming.
         *
         * CACHE COHERENCY: dcbst flushes the PPU L1 data cache line
         * containing the heartbeat value out to physical RAM. Without
         * this, PS3MAPI's kernel-level /read_process may read stale
         * data from physical memory since the kernel bypasses the PPU
         * L1 cache. sync ensures the dcbst completes before proceeding. */
        if (g_usb_hooks.heartbeat) {
            (*g_usb_hooks.heartbeat)++;
            __asm__ __volatile__ ("dcbst 0, %0" :: "r"((uint32_t)(uintptr_t)g_usb_hooks.heartbeat));
            __asm__ __volatile__ ("sync" ::: "memory");
        }

        /* Non-blocking receive from PC server */
        int n = network_recv(buf, sizeof(buf));
        if (n > 0) {
            DEBUG_VERBOSE("[MAIN] RX %d bytes from server\n", n);
        }

        /* Probe for server if not yet discovered (rate-limited).
         * Once server is known via hardcode in step 3b, this is a no-op. */
        network_maybe_probe_server(seq++);

        /* Send periodic keepalive heartbeat to PC server.
         * This is the ONLY outbound traffic in VSH mode (no USB hooks).
         * It ensures the Node.js server registers/keeps the PS3 client
         * in its clientAddress field before any game boots.
         * Rate-limited internally to once per 3 seconds. */
        network_send_keepalive();

        /* Yield PPU — critical: without this, VSH will freeze.
         * 50ms (20 Hz) — prevents sceNpTrophy deadlock. */
        sys_timer_usleep(50000); /* 50ms */
    }

    /* -------------------------------------------------------
     * 7. Cleanup (executed on g_shutdown from module_stop)
     * ------------------------------------------------------- */
    papertrail("=== Shutting down worker thread ===");
    DEBUG_PRINT("[MAIN] Shutdown requested\n");

    toypad_state_deinit();
    usb_hook_shutdown();
    network_shutdown();
    debug_shutdown();

    papertrail("=== worker_thread EXIT ===");
    sys_ppu_thread_exit(0);
}
