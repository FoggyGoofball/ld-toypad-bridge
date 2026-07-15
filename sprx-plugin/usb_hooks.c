/**
 * usb_hooks.c
 * USB hook manager - phase 1 scaffold.
 *
 * This file now tracks intended syscall hook targets and install state,
 * while preserving build stability until LV2 table patch writes are
 * reintroduced in a controlled phase.
 */

#include <string.h>
#include <stdlib.h>
#include <ppu-lv2.h>

#include "usb_hooks.h"
#include "debug.h"

#define SYSCALL_USBD_OPEN                845
#define SYSCALL_USBD_CLOSE               846
#define SYSCALL_USBD_GET_DESCRIPTOR      847
#define SYSCALL_USBD_CONTROL_TRANSFER    848
#define SYSCALL_USBD_INTERRUPT_TRANSFER  849

#ifndef USB_HOOK_ENABLE_PATCH
#define USB_HOOK_ENABLE_PATCH 0
#endif

#ifndef USB_HOOK_STRICT_PATCH
#define USB_HOOK_STRICT_PATCH 1
#endif

#ifndef USB_HOOK_SYSCALL_TABLE_BASE
#define USB_HOOK_SYSCALL_TABLE_BASE 0x8000000000000000ULL
#endif

#ifndef USB_HOOK_LV2_READ_SC
#define USB_HOOK_LV2_READ_SC 8
#endif

#ifndef USB_HOOK_LV2_WRITE_SC
#define USB_HOOK_LV2_WRITE_SC 9
#endif

typedef struct {
    const char* name;
    int syscall_num;
    int enabled;
} usb_hook_target_t;

static usb_hook_target_t g_targets[] = {
    {"sys_usbd_open",               SYSCALL_USBD_OPEN,               1},
    {"sys_usbd_close",              SYSCALL_USBD_CLOSE,              1},
    {"sys_usbd_get_descriptor",     SYSCALL_USBD_GET_DESCRIPTOR,     1},
    {"sys_usbd_control_transfer",   SYSCALL_USBD_CONTROL_TRANSFER,   1},
    {"sys_usbd_interrupt_transfer", SYSCALL_USBD_INTERRUPT_TRANSFER, 1},
};

static struct {
    int installed;
    int active_targets;
    int patched_targets;
} g_usb_hooks = {0};

typedef struct {
    int syscall_num;
    u64 hook_ptr;
    u64 original_ptr;
    int patched;
} hook_patch_entry_t;

static hook_patch_entry_t g_patch_entries[] = {
    {SYSCALL_USBD_OPEN,               0, 0, 0},
    {SYSCALL_USBD_CLOSE,              0, 0, 0},
    {SYSCALL_USBD_GET_DESCRIPTOR,     0, 0, 0},
    {SYSCALL_USBD_CONTROL_TRANSFER,   0, 0, 0},
    {SYSCALL_USBD_INTERRUPT_TRANSFER, 0, 0, 0},
};

static int str_eq(const char* a, const char* b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }

    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }

    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

// Forward declarations for hook functions provided by main.c.
extern int usbd_open_hook(uint32_t device_id, uint32_t* handle);
extern int usbd_close_hook(uint32_t device_id);
extern int usbd_get_descriptor_hook(uint32_t device_id, uint32_t type,
                                    uint32_t index, void* buffer, uint32_t* length);
extern int usbd_control_transfer_hook(uint32_t device_id, uint32_t bmRequestType,
                                      uint32_t bRequest, uint32_t wValue,
                                      uint32_t wIndex, void* data, uint32_t wLength);
extern int usbd_interrupt_transfer_hook(uint32_t device_id, uint32_t endpoint,
                                        void* buffer, uint32_t* length, uint32_t timeout);

static s64 lv2_read_kernel(u64 addr, void* buffer, u64 size)
{
    lv2syscall3(USB_HOOK_LV2_READ_SC, addr, (u64)buffer, size);
    return_to_user_prog(s64);
}

static s64 lv2_write_kernel(u64 addr, const void* buffer, u64 size)
{
    lv2syscall3(USB_HOOK_LV2_WRITE_SC, addr, (u64)buffer, size);
    return_to_user_prog(s64);
}

static int patch_config_valid(void)
{
    if (USB_HOOK_LV2_READ_SC <= 0 || USB_HOOK_LV2_WRITE_SC <= 0) {
        DEBUG_ERROR("[USB] Invalid LV2 syscall IDs (read=%d write=%d)\n",
                    USB_HOOK_LV2_READ_SC, USB_HOOK_LV2_WRITE_SC);
        return 0;
    }

    if (USB_HOOK_SYSCALL_TABLE_BASE == 0ULL) {
        DEBUG_ERROR("[USB] Invalid syscall table base (0)\n");
        return 0;
    }

    return 1;
}

static int read_syscall_ptr(int syscall_num, u64* out_ptr)
{
    u64 syscall_addr = USB_HOOK_SYSCALL_TABLE_BASE + ((u64)syscall_num * 8ULL);
    s64 ret;

    if (out_ptr == NULL) {
        return -1;
    }

    ret = lv2_read_kernel(syscall_addr, out_ptr, sizeof(*out_ptr));
    if (ret < 0) {
        DEBUG_ERROR("[USB] LV2 read failed for syscall %d (ret=%lld)\n",
                    syscall_num, (long long)ret);
        return -1;
    }

    return 0;
}

static int write_syscall_ptr_verified(int syscall_num, u64 target_ptr)
{
    u64 syscall_addr = USB_HOOK_SYSCALL_TABLE_BASE + ((u64)syscall_num * 8ULL);
    u64 verify_ptr = 0ULL;
    s64 ret;

    ret = lv2_write_kernel(syscall_addr, &target_ptr, sizeof(target_ptr));
    if (ret < 0) {
        DEBUG_ERROR("[USB] LV2 write failed for syscall %d (ret=%lld)\n",
                    syscall_num, (long long)ret);
        return -1;
    }

    if (read_syscall_ptr(syscall_num, &verify_ptr) < 0) {
        DEBUG_ERROR("[USB] LV2 verify read failed for syscall %d\n", syscall_num);
        return -1;
    }

    if (verify_ptr != target_ptr) {
        DEBUG_ERROR("[USB] LV2 verify mismatch for syscall %d (expected=0x%llX got=0x%llX)\n",
                    syscall_num, (unsigned long long)target_ptr, (unsigned long long)verify_ptr);
        return -1;
    }

    return 0;
}

static int patch_install_targets(void)
{
    int i;

    if (!patch_config_valid()) {
        return -1;
    }

    for (i = 0; i < (int)(sizeof(g_patch_entries) / sizeof(g_patch_entries[0])); i++) {
        if (read_syscall_ptr(g_patch_entries[i].syscall_num, &g_patch_entries[i].original_ptr) < 0) {
            return -1;
        }

        if (g_patch_entries[i].original_ptr == 0ULL || g_patch_entries[i].original_ptr == 0xFFFFFFFFFFFFFFFFULL) {
            DEBUG_ERROR("[USB] Invalid original target for syscall %d\n", g_patch_entries[i].syscall_num);
            return -1;
        }
    }

    for (i = 0; i < (int)(sizeof(g_patch_entries) / sizeof(g_patch_entries[0])); i++) {
        if (write_syscall_ptr_verified(g_patch_entries[i].syscall_num, g_patch_entries[i].hook_ptr) < 0) {
            return -1;
        }

        g_patch_entries[i].patched = 1;
        g_usb_hooks.patched_targets++;
    }

    return 0;
}

static void patch_remove_targets(void)
{
    int i;

    for (i = (int)(sizeof(g_patch_entries) / sizeof(g_patch_entries[0])) - 1; i >= 0; i--) {
        if (g_patch_entries[i].patched) {
            (void)write_syscall_ptr_verified(g_patch_entries[i].syscall_num, g_patch_entries[i].original_ptr);
            g_patch_entries[i].patched = 0;
        }
    }
    g_usb_hooks.patched_targets = 0;
}

int usb_hooks_install(void)
{
    int i;

    if (g_usb_hooks.installed) {
        DEBUG_PRINT("[USB] Hooks already installed (phase 1)\n");
        return 0;
    }

    g_usb_hooks.active_targets = 0;
    for (i = 0; i < (int)(sizeof(g_targets) / sizeof(g_targets[0])); i++) {
        if (g_targets[i].enabled) {
            g_usb_hooks.active_targets++;
            DEBUG_PRINT("[USB] Target: %s -> syscall %d\n",
                        g_targets[i].name, g_targets[i].syscall_num);
        }
    }

    DEBUG_PRINT("[USB] Phase 1 active. LV2 table writes are disabled in this build.\n");
    DEBUG_PRINT("[USB] Planned hook targets: %d\n", g_usb_hooks.active_targets);

    g_patch_entries[0].hook_ptr = (u64)(uintptr_t)usbd_open_hook;
    g_patch_entries[1].hook_ptr = (u64)(uintptr_t)usbd_close_hook;
    g_patch_entries[2].hook_ptr = (u64)(uintptr_t)usbd_get_descriptor_hook;
    g_patch_entries[3].hook_ptr = (u64)(uintptr_t)usbd_control_transfer_hook;
    g_patch_entries[4].hook_ptr = (u64)(uintptr_t)usbd_interrupt_transfer_hook;

    (void)usbd_open_hook;
    (void)usbd_close_hook;
    (void)usbd_get_descriptor_hook;
    (void)usbd_control_transfer_hook;
    (void)usbd_interrupt_transfer_hook;

#if USB_HOOK_ENABLE_PATCH
    DEBUG_PRINT("[USB] Phase 2 enabled: attempting LV2 syscall-table patch\n");
    DEBUG_PRINT("[USB] LV2 patch config read_sc=%d write_sc=%d table=0x%llX strict=%d\n",
                USB_HOOK_LV2_READ_SC,
                USB_HOOK_LV2_WRITE_SC,
                (unsigned long long)USB_HOOK_SYSCALL_TABLE_BASE,
                USB_HOOK_STRICT_PATCH);

    if (patch_install_targets() < 0) {
        patch_remove_targets();
        DEBUG_ERROR("[USB] Phase 2 patch failed.\n");

#if USB_HOOK_STRICT_PATCH
        DEBUG_ERROR("[USB] Strict mode active: aborting install\n");
        return -1;
#else
        DEBUG_ERROR("[USB] Strict mode disabled: falling back to phase 1 mode\n");
#endif
    } else {
        DEBUG_PRINT("[USB] Phase 2 patch active. Patched targets: %d\n", g_usb_hooks.patched_targets);
    }
#else
    DEBUG_PRINT("[USB] Phase 2 code compiled but disabled (USB_HOOK_ENABLE_PATCH=0)\n");
#endif

    g_usb_hooks.installed = 1;
    return 0;
}

int usb_hooks_remove(void)
{
    if (!g_usb_hooks.installed) {
        return 0;
    }

#if USB_HOOK_ENABLE_PATCH
    patch_remove_targets();
#endif

    g_usb_hooks.installed = 0;
    g_usb_hooks.active_targets = 0;
    g_usb_hooks.patched_targets = 0;
    DEBUG_PRINT("[USB] Hooks removed (phase 1)\n");
    return 0;
}

int get_syscall_number(const char* name)
{
    int i;

    if (name == NULL) {
        return -1;
    }

    for (i = 0; i < (int)(sizeof(g_targets) / sizeof(g_targets[0])); i++) {
        if (str_eq(name, g_targets[i].name)) {
            return g_targets[i].syscall_num;
        }
    }

    return -1;
}
