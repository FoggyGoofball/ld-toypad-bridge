/**
 * usb_hooks.h
 * USB syscall hook management for sys_usbd interception
 *
 * Provides the interface for installing/uninstalling LV2 syscall hooks
 * on the USB driver functions. Based on patterns from PS3XPAD.
 */

#ifndef USB_HOOKS_H
#define USB_HOOKS_H

#include <ppu-types.h>

/**
 * Install all USB hooks
 * Patches the LV2 kernel sys_usbd syscalls to redirect through our handlers.
 * Port filtering is done inside each hook to ensure Port 1 (SSD) is unaffected.
 *
 * @return 0 on success, negative on error
 */
int usb_hooks_install(void);

/**
 * Remove all USB hooks
 * Restores the original syscall table entries.
 * Must be called during _stop to clean up.
 *
 * @return 0 on success, negative on error
 */
int usb_hooks_remove(void);

/**
 * Check if a device ID corresponds to our target port (Port 2, right)
 * This is the core port isolation function.
 *
 * @param device_id USB device identifier from sys_usbd
 * @return 1 if this is Port 2 (Toy Pad target), 0 otherwise
 */
int is_toypad_port(uint32_t device_id);

/**
 * Syscall helper: Get the syscall number for a named function
 * This is needed because syscall numbers vary between CFW versions
 *
 * @param name Function name to look up
 * @return syscall number, or negative on error
 */
int get_syscall_number(const char* name);

#endif // USB_HOOKS_H
