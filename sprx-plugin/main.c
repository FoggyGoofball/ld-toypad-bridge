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

/* ================================================================
 * DIAGNOSTICS: g_init_progress — memory-mapped init tracker
 *
 * This volatile uint32_t is updated at EVERY init step boundary.
 * Its value tells us EXACTLY where the SPRX crashed (if it crashed).
 * The injector polls this address via PS3MAPI MEMORY READ.
 *
 * To find its address from outside: the SPRX writes &g_init_progress
 * to the progress file (ld_init_progress.txt) on the first successful
 * HDD write. The injector reads this file to learn the address, then
 * polls that address directly via PS3MAPI for real-time status.
 *
 * Steps:
 *   10 = module_start: thread created OK
 *   11 = module_start: returned 0 (resident)
 *   20 = worker_thread: ENTERED (first instruction)
 *   25 = worker_thread: Before debug_init()
 *   30 = worker_thread: After debug_init()
 *   35 = worker_thread: Before network_init()
 *   40 = worker_thread: After network_init()
 *   45 = worker_thread: Before network_wait_ready()
 *   50 = worker_thread: After network_wait_ready()
 *   55 = worker_thread: After network_set_server()
 *   60 = worker_thread: Before usb_hook_init() (trampoline allocation)
 *   61 = usb_hook_init: About to scan for cellUsbdInit GOT
 *   62 = usb_hook_init: About to scan for cellUsbdOpenPipe GOT
 *   63 = usb_hook_init: About to scan for cellUsbdInterruptTransfer GOT
 *   64 = usb_hook_init: About to scan for cellUsbdClosePipe GOT
 *   65 = worker_thread: sys_memory_allocate OK
 *   67 = worker_thread: create_hook_trampoline() done (4 trampolines)
 *   70 = worker_thread: usb_hook_init() returned
 *   75 = worker_thread: toypad_state_init() done
 *   80 = worker_thread: Entered main loop
 *   --- reboot signal: 999 = worker_thread EXIT ---
 * ================================================================ */
volatile uint32_t g_init_progress = 0;

/* Cache-coherent write: update g_init_progress AND flush L1/L2 cache
 * so the PS3MAPI /read_process poller sees the latest value immediately.
 *
 * NOTE: dcbst writes back from L1 data cache to L2. sync drain the
 * store buffer. The PS3MAPI kernel reads the physical address directly
 * (bypassing CPU caches), so the value MUST be in L2/DRAM. */
#define INIT_PROGRESS(x) do { \
    g_init_progress = (x); \
    __asm__ __volatile__ ("dcbst 0, %0\n\tsync" :: "r"(&g_init_progress) : "memory"); \
    debug_write_progress(); \
} while(0)


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

    /* FIXME-DIAG: remove once stable. Record g_init_progress address so
     * the PC injector can poll it via PS3MAPI memory read.
     *
     * Manual hex conversion avoids -lnosys_stub dependency (snprintf). */
    {
        int pfd;
        uint64_t pw;
        char pbuf[48];
        uint32_t addr = (uint32_t)(uintptr_t)&g_init_progress;
        const char* hex = "0123456789ABCDEF";
        int i = 0;
        int j;

        /* Write "INIT_PROGRESS_ADDR=0x" prefix */
        const char prefix[] = "INIT_PROGRESS_ADDR=0x";
        for (i = 0; prefix[i]; i++) pbuf[i] = prefix[i];

        /* Write hex address MSB-first (8 hex digits) */
        for (j = 28; j >= 0; j -= 4) {
            pbuf[i++] = hex[(addr >> j) & 0xF];
        }
        pbuf[i++] = '\n';
        pbuf[i] = '\0';

        if (cellFsOpen("/dev_hdd0/tmp/ld_init_progress.txt",
                       CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                       &pfd, NULL, 0) == CELL_OK) {
            cellFsWrite(pfd, pbuf, i, &pw);
            cellFsClose(pfd);
        }
    }


    papertrail("=== ldtoypad Full Integration: module_start ===");

    /* PS3MAPI-ONLY MODE: No VSH guard needed.
     * This SPRX is injected directly into the game process via
     * webMAN PS3MAPI MODULE LOAD. No boot_plugins.txt involved. */

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
    INIT_PROGRESS(10);
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

    /* Clean up any leftover VSH ready signal. */
    cellFsUnlink("/dev_hdd0/tmp/ld_vsh_ready.txt");

    papertrail("=== module_stop SUCCESS ===");
    return SYS_PRX_STOP_OK;
}

static void worker_thread(uint64_t arg)
{
    (void)arg;
    uint8_t seq = 0;

    /* ═══════════════════════════════════════════════════════════
     * ENTERED — FIRST INSTRUCTION in the new PPU thread.
     * If we crash anywhere before papertrail below, the boot.log
     * still won't help. The g_init_progress value will be stuck at
     * step 20, which the injector can poll via PS3MAPI MEMORY READ.
     * ═══════════════════════════════════════════════════════════ */
    INIT_PROGRESS(20);

    papertrail("=== worker_thread started ===");

    /* -------------------------------------------------------
     * 1. Initialize debug subsystem (ring buffer + HDD log)
     * ------------------------------------------------------- */
    INIT_PROGRESS(25);  /* Before debug_init */
    debug_init();
    INIT_PROGRESS(30);  /* After debug_init — debug_printf now works */
    papertrail("OK: debug_init()");

    DEBUG_PRINT("[MAIN] Worker thread entering init chain\n");

    /* ── Pre-network remote UDP logging ───────────────────────
     * Before network_init() binds the SPRX's UDP socket, we try
     * a raw sendto to the PC's IP (192.168.0.17:28473).
     *
     * This catches crashes that happen BEFORE the main network
     * socket is bound (e.g. inside network_init itself, or during
     * usb_hook_init before networking is operational).
     *
     * Port 28473 is reserved for this purpose — netcat on the PC:
     *   nc -u -l 0.0.0.0 28473
     * ──────────────────────────────────────────────────────── */
    debug_set_remote(htonl(0xC0A80011), 28473);
    DEBUG_PRINT("[MAIN] Pre-network UDP remote logging enabled (192.168.0.17:28473)\n");

    /* -------------------------------------------------------
     * 2. Initialize UDP network on port 28472
     * ------------------------------------------------------- */
    INIT_PROGRESS(35);  /* Before network_init */
    if (network_init(28472) != 0) {
        papertrail("FAIL: network_init() — fatal, cannot continue");
        DEBUG_ERROR("[MAIN] network_init() failed\n");
        debug_shutdown();
        sys_ppu_thread_exit(1);
        return;
    }
    INIT_PROGRESS(40);  /* After network_init */
    papertrail("OK: network_init(28472)");
    DEBUG_PRINT("[MAIN] UDP socket bound on port 28472\n");

    /* -------------------------------------------------------
     * 3. Wait for network interface (3s sleep heuristic)
     * ------------------------------------------------------- */
    INIT_PROGRESS(45);  /* Before network_wait_ready */
    papertrail("Waiting for network interface (3s)...");
    network_wait_ready();
    INIT_PROGRESS(50);  /* After network_wait_ready */
    papertrail("OK: network_wait_ready()");
    DEBUG_PRINT("[MAIN] Network interface ready\n");

    /* -------------------------------------------------------
     * 3b. Hardcode PC server IP (dev phase — bypass broadcast)
     *     PC=192.168.0.17:28472. Also redirect remote UDP target
     *     to 28472 (since both sockets now exist on the PC).
     * ------------------------------------------------------- */
    network_set_server(htonl(0xC0A80011), 28472);
    debug_set_remote(htonl(0xC0A80011), 28472);
    INIT_PROGRESS(55);  /* After network_set_server */
    papertrail("OK: network_set_server(192.168.0.17:28472)");
    DEBUG_PRINT("[MAIN] Server hardcoded to 192.168.0.17:28472\n");

    /* -------------------------------------------------------
     * 4. Install USB detour hooks for Toy Pad emulation.
     *    Gracefully fails if not in game process (returns -1).
     *
     *    THIS IS THE MOST LIKELY CRASH POINT:
     *    - sys_memory_allocate for R-W-X trampoline page
     *    - NID scans of the game's cellUsbd GOT entries
     *    - Writing PowerPC trampoline instructions to exec memory
     *    - Flushing instruction cache (dcbst/icbi)
     *    - Overwriting game's GOT slots with trampoline addresses
     * ------------------------------------------------------- */
    INIT_PROGRESS(60);  /* Before usb_hook_init */
    {
        int hooks_ok = usb_hook_init();
        INIT_PROGRESS(70);  /* After usb_hook_init returned */
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
    INIT_PROGRESS(75);  /* Before toypad_state_init */
    toypad_state_init();
    papertrail("OK: toypad_state_init()");
    DEBUG_PRINT("[MAIN] Toy Pad state machine ready\n");

    /* -------------------------------------------------------
     * 6. Main background loop
     * ------------------------------------------------------- */
    INIT_PROGRESS(80);  /* Entering main loop */
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
