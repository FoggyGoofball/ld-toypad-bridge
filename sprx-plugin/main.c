#include <ppu-types.h>
#include <sys/prx.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <lv2/sysfs.h>
#include <string.h>

#include "usb_hooks.h"
#include "network.h"
#include "toypad_state.h"
#include "debug.h"

#define CONFIG_UDP_PORT 28472
#define PORT_LEFT_SSD 1
#define PORT_RIGHT_TOYPAD 2
#define LDTP_ENABLE_FLAG_PATH "/dev_hdd0/plugins/ldtoypad.enable"
#define LDTP_ENABLE_FLAG_MAX_AGE_SEC 900

#ifndef USB_HOOK_ENABLE_PATCH
#define USB_HOOK_ENABLE_PATCH 0
#endif

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

static struct {
    int initialized;
} g_state = {0};

static int plugin_enabled(void)
{
    sysFSStat stat;

    return (sysFsStat(LDTP_ENABLE_FLAG_PATH, &stat) == 0) ? 1 : 0;
}

static int check_enable_flag(void)
{
    sysFSStat stat;

    if (sysFsStat(LDTP_ENABLE_FLAG_PATH, &stat) != 0) {
        return 0;
    }

    DEBUG_PRINT("[LDTP] enable flag present at %s\n", LDTP_ENABLE_FLAG_PATH);
    return 1;
}

int is_toypad_port(u32 device_id)
{
    u8 port = device_id & 0x0F;

    if (port == PORT_LEFT_SSD) {
        return 0;
    }

    if (port == PORT_RIGHT_TOYPAD) {
        return 1;
    }

    return 0;
}

int usbd_open_hook(uint32_t device_id, uint32_t* handle)
{
    if (!is_toypad_port(device_id)) {
        return 0;
    }

    toypad_state_set_device_id(device_id);
    if (handle) {
        *handle = device_id;
    }

    DEBUG_PRINT("[LDTP] usbd_open_hook dev=0x%08X\n", device_id);
    return 0;
}

int usbd_close_hook(uint32_t device_id)
{
    if (!is_toypad_port(device_id)) {
        return 0;
    }

    DEBUG_PRINT("[LDTP] usbd_close_hook dev=0x%08X\n", device_id);
    return 0;
}

int usbd_get_descriptor_hook(uint32_t device_id, uint32_t type,
                             uint32_t index, void* buffer, uint32_t* length)
{
    if (!is_toypad_port(device_id)) {
        return 0;
    }

    return toypad_state_get_descriptor(type, index, buffer, length);
}

int usbd_control_transfer_hook(uint32_t device_id, uint32_t bmRequestType,
                               uint32_t bRequest, uint32_t wValue,
                               uint32_t wIndex, void* data, uint32_t wLength)
{
    if (!is_toypad_port(device_id)) {
        return 0;
    }

    return toypad_state_control_transfer(bmRequestType, bRequest, wValue,
                                         wIndex, data, wLength);
}

int usbd_interrupt_transfer_hook(uint32_t device_id, uint32_t endpoint,
                                 void* buffer, uint32_t* length, uint32_t timeout)
{
    if (!is_toypad_port(device_id)) {
        return 0;
    }

    if (endpoint == EP_INTERRUPT_IN) {
        return toypad_state_handle_interrupt_in(endpoint, buffer, length, timeout);
    }

    if (endpoint == EP_INTERRUPT_OUT) {
        uint32_t out_len = (length != NULL) ? *length : 0;
        return toypad_state_handle_interrupt_out(endpoint, buffer, out_len);
    }

    return -1;
}

int _start(u64 args)
{
    (void)args;

    if (g_state.initialized) {
        return SYS_PRX_START_OK;
    }

    if (!plugin_enabled()) {
        DEBUG_PRINT("[LDTP] plugin dormant: create %s to enable runtime\n", LDTP_ENABLE_FLAG_PATH);
        return SYS_PRX_START_OK;
    }

    if (!check_enable_flag()) {
        DEBUG_PRINT("[LDTP] plugin dormant: %s not found\n", LDTP_ENABLE_FLAG_PATH);
        return SYS_PRX_START_OK;
    }

    debug_init();
    toypad_state_init();

    if (network_init(CONFIG_UDP_PORT) < 0) {
        DEBUG_PRINT("[LDTP] network_init failed\n");
    }

    // Install USB hooks (may fail on wrong syscall table base).
    if (usb_hooks_install() < 0) {
        DEBUG_PRINT("[LDTP] usb_hooks_install failed — plugin will not intercept USB\n");
        DEBUG_PRINT("[LDTP] Keeping network alive so server can still detect PS3\n");
    }

#if USB_HOOK_ENABLE_PATCH
    DEBUG_PRINT("[LDTP] USB phase2 patch path: ENABLED\n");
#else
    DEBUG_PRINT("[LDTP] USB phase2 patch path: DISABLED (safe default)\n");
#endif

    g_state.initialized = 1;
    DEBUG_PRINT("[LDTP] plugin started on UDP %d\n", CONFIG_UDP_PORT);
    return SYS_PRX_START_OK;
}

int _stop(void)
{
    if (!g_state.initialized) {
        return SYS_PRX_STOP_OK;
    }

    usb_hooks_remove();
    network_shutdown();
    toypad_state_deinit();
    debug_shutdown();

    g_state.initialized = 0;
    return SYS_PRX_STOP_OK;
}
