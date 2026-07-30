/**
 * opd_hooks.c — Callback Stealing + OPD Hook Implementation
 *
 * Expert strategy:
 *   1. Hook cellUsbdRegisterLdd → steal game's CellUsbdLddOps
 *   2. Hook cellUsbdGetDeviceDescriptor → fake ToyPad VID/PID
 *   3. Hook cellUsbdBulkTransfer → MITM HID data
 *   4. Manually fire game's probe()/attach() from worker thread
 */

#include <stdint.h>
#include <string.h>
#include <cell/usbd.h>
#include <cell/usbd/libusbd.h>

#include "opd_hooks.h"
#include "debug.h"

/* ── Stolen game callbacks ── */
static CellUsbdLddOps *g_game_ops = NULL;
static uint32_t        g_game_toc = 0;

#define FAKE_TOYPAD_DEV_ID  0x9999
#define BRIDGE_BUF_SIZE 64

/* ── Bridge buffers ── */
static uint8_t  g_in_buf[BRIDGE_BUF_SIZE];
static volatile int g_in_len = 0, g_in_ready = 0;
static uint8_t  g_out_buf[BRIDGE_BUF_SIZE];
static volatile int g_out_len = 0, g_out_new = 0;
static int g_hooks_active = 0;

static const uint8_t g_toypad_desc[] = {
    0x12,0x01,0x00,0x02,0x00,0x00,0x00,0x40,
    0x6F,0x0E,0x41,0x02,0x00,0x01,0x01,0x02,0x00,0x01
};

/* ================================================================
 * Hook: cellUsbdRegisterLdd — steal game's callbacks
 * ================================================================ */
int32_t hook_cellUsbdRegisterLdd_hook(CellUsbdLddOps *ops)
{
    if (ops && !g_game_ops) {
        g_game_ops = ops;
        DEBUG_PRINT("[OPD] *** STOLE game LDD! probe=0x%08X attach=0x%08X ***\n",
                    (uint32_t)(uintptr_t)ops->probe,
                    (uint32_t)(uintptr_t)ops->attach);
    }
    return 0;
}

/* ================================================================
 * Hook: cellUsbdBulkTransfer — HID MITM
 * ================================================================ */
int32_t hook_cellUsbdInterruptTransfer(int32_t pipe_id, void *buf,
                                        int32_t len,
                                        CellUsbdDoneCallback done_cb,
                                        void *arg)
{
    int is_out = ((uint32_t)pipe_id & 0xFF) == 0x01;
    int is_in  = ((uint32_t)pipe_id & 0xFF) == 0x81;

    if (is_out && buf && len > 0 && len <= BRIDGE_BUF_SIZE) {
        memcpy((void *)g_out_buf, buf, (size_t)len);
        g_out_len = len; g_out_new = 1;
        DEBUG_PRINT("[OPD] OUT: captured %d bytes\n", len);
    }
    if (is_in && buf && len > 0) {
        if (g_in_ready && g_in_len > 0) {
            int c = (g_in_len < len) ? g_in_len : len;
            memcpy(buf, (void *)g_in_buf, (size_t)c);
            if (c < len) memset((uint8_t*)buf + c, 0, (size_t)(len - c));
            g_in_ready = 0;
            DEBUG_PRINT("[OPD] IN: injected %d bytes\n", c);
            if (done_cb) done_cb(0, c, arg);
            return 0;
        }
        memset(buf, 0, (size_t)len);
        if (done_cb) done_cb(0, 0, arg);
        return 0;
    }
    return -1;
}

/* ── Public API ── */
int opd_hooks_has_game_ops(void) { return g_game_ops && g_game_toc; }

int opd_hooks_fire_hotplug(void)
{
    if (!g_game_ops || !g_game_toc) return -1;

    DEBUG_PRINT("[OPD] Firing game probe(%d)...\n", FAKE_TOYPAD_DEV_ID);

    opd_t popd; popd.func_addr = (uint32_t)(uintptr_t)g_game_ops->probe;
    popd.toc_addr = g_game_toc;
    int32_t (*probe)(int32_t) = (int32_t(*)(int32_t))&popd;
    int32_t r = probe(FAKE_TOYPAD_DEV_ID);
    DEBUG_PRINT("[OPD] probe returned 0x%08X\n", r);
    if (r < 0) return -1;

    opd_t aopd; aopd.func_addr = (uint32_t)(uintptr_t)g_game_ops->attach;
    aopd.toc_addr = g_game_toc;
    int32_t (*attach)(int32_t) = (int32_t(*)(int32_t))&aopd;
    attach(FAKE_TOYPAD_DEV_ID);
    DEBUG_PRINT("[OPD] *** HOTPLUG FIRED! Game thinks ToyPad connected! ***\n");
    return 0;
}

int opd_hooks_get_out_data(uint8_t *o, int m) {
    if (!g_out_new) return 0;
    int c = (g_out_len < m) ? g_out_len : m;
    memcpy(o, (void*)g_out_buf, (size_t)c); g_out_new = 0; return c;
}
void opd_hooks_push_in_data(const uint8_t *d, int n) {
    if (n > BRIDGE_BUF_SIZE) n = BRIDGE_BUF_SIZE;
    memcpy((void*)g_in_buf, d, (size_t)n); g_in_len = n; g_in_ready = 1;
}
int opd_hooks_are_active(void) { return g_hooks_active; }

/* ================================================================
 * Self-Resolution Scan
 * ================================================================ */
int opd_hooks_init(void)
{
    struct { const char *name; void *fn; uint32_t addr; int h; } t[] = {
        {"cellUsbdRegisterLdd",         &cellUsbdRegisterLdd,         0,0},
        {"cellUsbdRegisterExtraLdd",    &cellUsbdRegisterExtraLdd,    0,0},
        {"cellUsbdControlTransfer",     &cellUsbdControlTransfer,     0,0},
        {"cellUsbdBulkTransfer",        &cellUsbdBulkTransfer,        0,0},
    };
    int n = sizeof(t)/sizeof(t[0]), i, installed = 0;

    DEBUG_PRINT("[OPD] Callback stealing scan...\n");
    for (i=0; i<n; i++) {
        opd_t *o = (opd_t*)t[i].fn; t[i].addr = o->func_addr;
        DEBUG_PRINT("[OPD] %s: 0x%08X\n", t[i].name, o->func_addr);
    }

    for (int c=0; c<2 && installed < n; c++) {  /* stop when all targets hooked */
        uint32_t *s = (uint32_t*)(0x00010000 + c*0x02000000);
        uint32_t *e = (uint32_t*)(0x00010000 + (c+1)*0x02000000);
        DEBUG_PRINT("[OPD] Chunk %d: 0x%08X-0x%08X\n", c, (uint32_t)(uintptr_t)s, (uint32_t)(uintptr_t)e);
        for (uint32_t *p=s; p<e; p++) {
            if (*p==0) continue;
            for (i=0; i<n; i++) {
                if (t[i].h) continue;
                if (*p != t[i].addr) continue;
                uint32_t nv = *(p+1);
                if (nv <= 0x00010000 || nv >= 0x20000000) continue;

                opd_t *hop;
                if (i==3) hop = (opd_t*)&hook_cellUsbdInterruptTransfer;     /* Bulk */
                else if (i==2) hop = (opd_t*)&hook_cellUsbdInterruptTransfer; /* Ctrl */
                else hop = (opd_t*)&hook_cellUsbdRegisterLdd_hook;           /* 0,1 */

                if (i==0 || i==1) {
                    g_game_toc = nv;
                    DEBUG_PRINT("[OPD] Game TOC: 0x%08X\n", g_game_toc);
                }

                p[0] = hop->func_addr; p[1] = hop->toc_addr;
                t[i].h = 1; installed++;
                DEBUG_PRINT("[OPD] *** HOOKED %s at 0x%08X ***\n", t[i].name, (uint32_t)(uintptr_t)p);
                if (installed >= n) goto scan_done;  /* all found — stop immediately */
            }
        }
    }

scan_done:
    if (installed) {
        g_hooks_active = 1;
        DEBUG_PRINT("[OPD] %d hooks installed\n", installed);

        /* If RegisterLdd hook didn't fire (game registered before we
         * injected), scan memory for the game's CellUsbdLddOps struct.
         * The struct is {name_ptr, probe_OPD, attach_OPD, detach_OPD}.
         * All OPD entries share the game's TOC (0x2C38340). */
        if (!g_game_ops && g_game_toc != 0) {
            DEBUG_PRINT("[OPD] Scanning for game ops near OPD cluster...\n");
            /* Narrow scan: 8KB around the known OPD cluster (0x2C2F000-0x2C31000) */
            uint32_t *s = (uint32_t*)0x02C2F000;
            uint32_t *e = (uint32_t*)0x02C31000;
            for (uint32_t *p = s; p < e - 3; p++) {
                uint32_t name_ptr = p[0];
                if (name_ptr < 0x00010000 || name_ptr > 0x20000000) continue;
                /* Must be 8-byte aligned (OPD alignment) */
                if ((uint32_t)(uintptr_t)p & 0x7) continue;
                /* Check p[1..3] are OPDs with game TOC */
                if (p[1]==0 || p[2]==0 || p[3]==0) continue;
                /* Bounds check: OPD pointers must be in mapped user range */
                if (p[1] < 0x10000 || p[1] > 0x0FFFFFFF) continue;
                if (p[2] < 0x10000 || p[2] > 0x0FFFFFFF) continue;
                if (p[3] < 0x10000 || p[3] > 0x0FFFFFFF) continue;
                uint32_t t1 = *(uint32_t*)(p[1] + 4);
                uint32_t t2 = *(uint32_t*)(p[2] + 4);
                uint32_t t3 = *(uint32_t*)(p[3] + 4);
                if (t1 != g_game_toc || t2 != g_game_toc || t3 != g_game_toc) continue;

                g_game_ops = (CellUsbdLddOps*)p;
                DEBUG_PRINT("[OPD] *** FOUND game ops at 0x%08X ***\n", (uint32_t)(uintptr_t)p);
                break;
            }
            if (!g_game_ops)
                DEBUG_PRINT("[OPD] ops scan: not found in 0x2C2F000-0x2C31000\n");
        }

        return 0;
    }
    DEBUG_ERROR("[OPD] No hooks installed\n");
    return -1;
}

void opd_hooks_shutdown(void) { g_hooks_active = 0; g_game_ops = NULL; }
