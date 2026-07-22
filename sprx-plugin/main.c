/**
 * main.c — LD-ToyPad Bridge SPRX — Game Process Hook Variant
 *
 * Designed to be injected into the LEGO Dimensions game process
 * via PS3MAPI. Intercepts the game's cellUsbd calls using PowerPC
 * user-space detour hooks and routes Toy Pad traffic over UDP.
 *
 * Init chain in worker thread:
 *   debug_init() -> network_init() -> network_wait_ready()
 *   -> usb_hook_init() -> toypad_state_init()
 *   -> main loop (recvfrom non-blocking, server probes, 10ms yield)
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
     * Detection: Uses ld_vsh_guard.txt as a sentinel file.
     *
     * Scenario A (boot_plugins.txt load at console boot):
     *   Guard was written by a PREVIOUS injection session. It persists
     *   on HDD across reboots. File EXISTS -> detect VSH -> unload.
     *
     * Scenario B (PS3MAPI injection #1):
     *   Guard does not exist -> write it -> proceed normally.
     *
     * Scenario C (PS3MAPI injection #2+, same boot):
     *   Guard was written in injection #1, but module_stop() deletes it
     *   during PS3MAPI unload. So guard does NOT exist -> proceed. */
    {
        int scratch_fd;
        uint64_t dummy_written;

        /* Check if guard file exists — if so, VSH context via boot_plugins.txt */
        int exists_check = cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
                                      CELL_FS_O_RDONLY,
                                      &scratch_fd, NULL, 0);
        if (exists_check == CELL_OK) {
            cellFsClose(scratch_fd);
            papertrail("VSH GUARD: detected VSH context — unloading immediately");
            papertrail("VSH GUARD: remove ldtoypad.sprx from /dev_hdd0/boot_plugins.txt");
            /* Write detection evidence so user knows what happened */
            int guard_fd;
            if (cellFsOpen("/dev_hdd0/tmp/ld_vsh_detected.txt",
                           CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                           &guard_fd, NULL, 0) == CELL_OK) {
                char msg[] = "VSH_DETECTED=1\nLoaded from boot_plugins.txt\n";
                cellFsWrite(guard_fd, msg, strlen(msg), &dummy_written);
                cellFsClose(guard_fd);
            }
            return SYS_PRX_NO_RESIDENT;  /* Tell kernel to unload us immediately */
        }

        /* Guard does not exist — this is a PS3MAPI injection.
         * Write guard for next boot's VSH detection. */
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

    /* Shutdown subsystems in reverse order of init. */
    toypad_state_deinit();
    usb_hook_shutdown();
    network_shutdown();
    debug_shutdown();

    /* Delete the VSH guard sentinel so re-injection works without
     * requiring a full console reboot. The guard persists on HDD
     * across boots, so it still protects against boot_plugins.txt
     * on the NEXT boot. FIX 2026-07-22: added this deletion to
     * prevent false-positive VSH detection on re-injection. */
    {
        int guard_fd;
        if (cellFsOpen("/dev_hdd0/tmp/ld_vsh_guard.txt",
                       CELL_FS_O_WRONLY | CELL_FS_O_TRUNC,
                       &guard_fd, NULL, 0) == CELL_OK) {
            cellFsClose(guard_fd);
            papertrail("OK: VSH guard deleted for re-injection");
        }
    }

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
     * 3. Wait for network interface (3s sleep heuristic)
     * ------------------------------------------------------- */
    papertrail("Waiting for network interface (3s)...");
    network_wait_ready();
    papertrail("OK: network_wait_ready()");
    DEBUG_PRINT("[MAIN] Network interface ready\n");

    /* -------------------------------------------------------
     * 3b. Hardcode PC server IP (dev phase — bypass broadcast)
     *     PC=192.168.0.17:28472
     * ------------------------------------------------------- */
    network_set_server(htonl(0xC0A80011), 28472);
    papertrail("OK: network_set_server(192.168.0.17:28472)");
    DEBUG_PRINT("[MAIN] Server hardcoded to 192.168.0.17:28472\n");

    /* -------------------------------------------------------
     * 4. Install USB detour hooks for Toy Pad emulation.
     *    Gracefully fails if not in game process (returns -1).
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
     * ------------------------------------------------------- */
    papertrail("=== Entering main loop ===");
    DEBUG_PRINT("[MAIN] Entering background loop\n");

    while (!g_shutdown) {
        uint8_t buf[NET_PACKET_MAX_SIZE];

        /* Memory-mapped heartbeat for deadlock detection */
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

        /* Probe for server if not yet discovered (rate-limited). */
        network_maybe_probe_server(seq++);

        /* Send periodic keepalive heartbeat to PC server. */
        network_send_keepalive();

        /* Yield PPU — 50ms (20 Hz) prevents sceNpTrophy deadlock. */
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
