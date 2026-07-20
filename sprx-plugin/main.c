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
 * -lusbd_stub NOT linked — cellUsbd calls are intercepted via hooks,
 *   not called directly.
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
#include "hook.h"
#include "usb_hooks.h"
#include "toypad_state.h"

SYS_MODULE_INFO(ldtoypad, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);

static void worker_thread(uint64_t arg);

static volatile int g_shutdown = 0;
static sys_ppu_thread_t g_worker_tid = SYS_PPU_THREAD_ID_INVALID;

/** Write a line to /dev_hdd0/plugins/ldtoypad_boot.log */
static int papertrail(const char *msg)
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

    papertrail("Creating worker thread...");
    ret = sys_ppu_thread_create(&g_worker_tid,
                                (void(*)(uint64_t))worker_thread,
                                0, 1000, 16*1024,
                                SYS_PPU_THREAD_CREATE_JOINABLE,
                                "ldtoypad_worker");
    if (ret != CELL_OK) {
        papertrail("FAIL: thread create");
        return 1;
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
     *    cellUsbdTransfer, and cellUsbdClosePipe functions.
     *    Routes Toy Pad USB traffic to the Node.js server via UDP.
     *
     *    This requires the SPRX to be loaded into the game process
     *    (via PS3MAPI injection). If running in VSH context,
     *    hook installation will fail gracefully.
     *
     *    Pass NULL for PRX TOC — usb_hook_init will fail gracefully
     *    (find_game_cellusbd_functions placeholder always returns -1),
     *    logging that we're in VSH-only network mode.  In production
     *    with PS3MAPI injection, the actual TOC address would come
     *    from the PRX loader.
     * ------------------------------------------------------- */
    {
        int hooks_ok = usb_hook_init(NULL);
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
     *      - 10ms sleep each iteration to yield PPU to VSH
     *
     *    The actual USB interrupt polling will be driven by the
     *    CellOS USB callback system once a device is attached.
     *    Until then, this loop keeps the network path alive.
     * ------------------------------------------------------- */
    papertrail("=== Entering main loop ===");
    DEBUG_PRINT("[MAIN] Entering background loop\n");

    while (!g_shutdown) {
        uint8_t buf[NET_PACKET_MAX_SIZE];

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

        /* Yield PPU — critical: without this, VSH will freeze */
        sys_timer_usleep(10000); /* 10ms */
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
