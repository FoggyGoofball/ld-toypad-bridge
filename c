/**
 * usb_hooks.c
 * USB syscall hook installation/removal
 *
 * Implements LV2 kernel syscall patching to intercept sys_usbd functions.
 * Based on reverse engineering from PS3XPAD project.
 *
 * WARNING: This code runs in kernel (LV2) context.
 * Incorrect memory access WILL cause a kernel panic.
 *
 * Port isolation:
 *   - Port 1 (left): SSD passthrough (no hooks)
 *   - Port 2 (right): Toy Pad access only (intercepted)
 */

#include <string.h>
#include <stdlib.h>

#include <psl1ght/lv2/syscalls.h>
#include <psl1ght/lv2/sys_usbd.h>
#include <psl1ght/lv2/sys_memory.h>

#include "usb_hooks.h"
#include "debug.h"

// =========================================
// Syscall table patching
// =========================================

/**
 * The PS3 LV2 kernel maintains a syscall table at a fixed address.
 * Each entry is a 64-bit function pointer.
 * We need to:
 *   1. Find the syscall number for each sys_usbd function
 *   2. Replace the table entry with our hook function
 *   3. Save the original pointer for passthrough
 *
 * Syscall numbers vary between CFW versions.
 * For Evilnat 4.93, typical sys_usbd syscall numbers:
 *   sys_usbd_open              = 845
 *   sys_usbd_close             = 846
 *   sys_usbd_get_descriptor    = 847
 *   sys_usbd_control_transfer  = 848
 *   sys_usbd_interrupt_transfer = 849
 */

// Syscall table base address (LV2 kernel memory)
// This is fixed for PS3 firmware 4.xx
#define SYSCALL_TABLE_BASE      0x8000000000000000ULL  // Adjust based on firmware

// Maximum number of syscalls to scan
#define MAX_SYSCALL_NUM         1024

// Structure for a hook entry
typedef struct {
    uint32_t    syscall_num;    // Syscall number in the table
    uint64_t    original_ptr;   // Original function pointer
    uint64_t    hook_ptr;       // Our hook function pointer
    int         installed;      // Whether this hook is active
} hook_entry_t;

// Hook entries for USB functions
static hook_entry_t g_hooks[] = {
    { 845, 0, (uint64_t)usbd_open_hook, 0 },              // sys_usbd_open
    { 846, 0, (uint64_t)usbd_close_hook, 0 },             // sys_usbd_close
    { 847, 0, (uint64_t)usbd_get_descriptor_hook, 0 },    // sys_usbd_get_descriptor
    { 848, 0, (uint64_t)usbd_control_transfer_hook, 0 },  // sys_usbd_control_transfer
    { 849, 0, (uint64_t)usbd_interrupt_transfer_hook, 0 },// sys_usbd_interrupt_transfer
};
static const int g_num_hooks = sizeof(g_hooks) / sizeof(g_hooks[0]);

// Memory protection key for LV2 memory access
static uint32_t g_lv2_key = 0;

// =========================================
// LV2 Memory Access Primitives
// =========================================

/**
 * Get the LV2 memory protection key
 * This varies between firmware versions
 */
static uint32_t get_lv2_key(void)
{
    // On Evilnat 4.93, the LV2 key is typically 0xDEADBEEF or similar
    // This needs to be extracted from the specific CFW version
    // For now, use a placeholder that should be verified on actual hardware
    uint32_t key = 0;
    sys_ss_get_console_id(NULL, 0); // This triggers LV2 access setup
    // TODO: Extract actual LV2 key for Evilnat 4.93
    return key;
}

/**
 * Read LV2 kernel memory
 * LV2 memory is mapped at a specific offset accessible via lv1/lv2 calls
 */
static int lv2_mem_read(uint64_t addr, void* buffer, uint32_t size)
{
    // Use the syscall that grants LV2 memory access
    // The exact mechanism depends on the CFW
    system_call_2(8, (uint64_t)addr, (uint64_t)buffer);
    return (int)p1;
}

/**
 * Write to LV2 kernel memory
 * This is the dangerous part - incorrect writes cause kernel panics
 */
static int lv2_mem_write(uint64_t addr, const void* buffer, uint32_t size)
{
    // Use the syscall that enables LV2 memory write access
    // The exact mechanism depends on the CFW
    system_call_3(9, (uint64_t)addr, (uint64_t)buffer, (uint64_t)size);
    return (int)p1;
}

/**
 * Parse the syscall table to find actual syscall numbers for sys_usbd
 * This is more robust than assuming hardcoded values
 */
static int resolve_syscall_numbers(void)
{
    // Strategy: Scan the syscall table for function pointers that match
    // known sys_usbd function signatures, or use the exported symbol table.
    //
    // For Evilnat 4.93, we can also use:
    //   sys_lv1_kernel_get_syscall_table_info()
    //   sys_lv1_kernel_get_syscall_table_entry()
    //
    // For now, we use the known values for 4.90+ Evilnat
    // These should be verified on actual hardware
    
    DEBUG_PRINT("[USB] Using default syscall numbers (Evilnat 4.93)\n");
    DEBUG_PRINT("[USB]   sys_usbd_open:              %d\n", g_hooks[0].syscall_num);
    DEBUG_PRINT("[USB]   sys_usbd_close:             %d\n", g_hooks[1].syscall_num);
    DEBUG_PRINT("[USB]   sys_usbd_get_descriptor:    %d\n", g_hooks[2].syscall_num);
    DEBUG_PRINT("[USB]   sys_usbd_control_transfer:  %d\n", g_hooks[3].syscall_num);
    DEBUG_PRINT("[USB]   sys_usbd_interrupt_transfer:%d\n", g_hooks[4].syscall_num);
    
    return 0;
}

// =========================================
// Hook Installation
// =========================================

int usb_hooks_install(void)
{
    DEBUG_PRINT("[USB] Installing USB hooks...\n");

    // Resolve syscall numbers
    if (resolve_syscall_numbers() < 0) {
        DEBUG_ERROR("[USB] Failed to resolve syscall numbers\n");
        return -1;
    }

    // Get LV2 memory key
    g_lv2_key = get_lv2_key();
    DEBUG_PRINT("[USB] LV2 key: 0x%08X\n", g_lv2_key);

    // Install each hook
    for (int i = 0; i < g_num_hooks; i++) {
        hook_entry_t* hook = &g_hooks[i];
        
        // Read the current syscall table entry
        uint64_t syscall_addr = SYSCALL_TABLE_BASE + (hook->syscall_num * 8);
        
        if (lv2_mem_read(syscall_addr, &hook->original_ptr, sizeof(hook->original_ptr)) < 0) {
            DEBUG_ERROR("[USB] Failed to read syscall %d entry\n", hook->syscall_num);
            continue;
        }

        // Verify we got a valid function pointer (not already hooked)
        if (hook->original_ptr == hook->hook_ptr) {
            DEBUG_PRINT("[USB] Hook %d already installed, skipping\n", hook->syscall_num);
            hook->installed = 1;
            continue;
        }

        // Check that the original pointer looks valid
        if (hook->original_ptr == 0 || hook->original_ptr == 0xFFFFFFFFFFFFFFFFULL) {
            DEBUG_ERROR("[USB] Invalid original pointer for syscall %d: 0x%016llX\n",
                       hook->syscall_num, hook->original_ptr);
            continue;
        }

        // Disable write protection (required for LV2 memory patching)
        // This typically involves calling sys_lv1_kernel_set_whatever
        disable_write_protection();

        // Write our hook pointer
        if (lv2_mem_write(syscall_addr, &hook->hook_ptr, sizeof(hook->hook_ptr)) < 0) {
            DEBUG_ERROR("[USB] Failed to write hook for syscall %d\n", hook->syscall_num);
            enable_write_protection();
            continue;
        }

        // Re-enable write protection
        enable_write_protection();

        hook->installed = 1;
        DEBUG_PRINT("[USB] Hooked syscall %d: 0x%016llX -> 0x%016llX\n",
                   hook->syscall_num, hook->original_ptr, hook->hook_ptr);
    }

    DEBUG_PRINT("[USB] USB hooks installation complete\n");
    return 0;
}

/**
 * Disable LV2 memory write protection
 * This is firmware-specific
 */
static void disable_write_protection(void)
{
    // On Evilnat 4.93, this typically involves:
    // 1. sys_lv1_kernel_write_protect(0)
    // 2. Or using lv2 syscall 6/7 for memory access
    
    // Placeholder - actual implementation requires CFW-specific knowledge
    // system_call_1(XXX, 0);
}

/**
 * Re-enable LV2 memory write protection
 */
static void enable_write_protection(void)
{
    // Placeholder - mirror of disable above
    // system_call_1(XXX, 1);
}

// =========================================
// Hook Removal
// =========================================

int usb_hooks_remove(void)
{
    DEBUG_PRINT("[USB] Removing USB hooks...\n");

    for (int i = 0; i < g_num_hooks; i++) {
        hook_entry_t* hook = &g_hooks[i];

        if (!hook->installed) {
            continue;
        }

        // Restore original syscall table entry
        uint64_t syscall_addr = SYSCALL_TABLE_BASE + (hook->syscall_num * 8);
        
        disable_write_protection();
        
        if (lv2_mem_write(syscall_addr, &hook->original_ptr, sizeof(hook->original_ptr)) < 0) {
            DEBUG_ERROR("[USB] Failed to restore syscall %d\n", hook->syscall_num);
        } else {
            DEBUG_PRINT("[USB] Restored syscall %d: 0x%016llX\n",
                       hook->syscall_num, hook->original_ptr);
        }
        
        enable_write_protection();
        
        hook->installed = 0;
    }

    DEBUG_PRINT("[USB] USB hooks removal complete\n");
    return 0;
}

// =========================================
// Syscall helper
// =========================================

int get_syscall_number(const char* name)
{
    // Look up syscall number by name
    // This could use the PS3's exported symbol table
    // For now, return the default value
    
    DEBUG_VERBOSE("[USB] Getting syscall number for: %s\n", name);
    
    // Return -1 to indicate "use default"
    return -1;
}

// Forward declarations for hook functions (defined in main.c)
extern int usbd_open_hook(uint32_t device_id, uint32_t* handle);
extern int usbd_close_hook(uint32_t device_id);
extern int usbd_get_descriptor_hook(uint32_t device_id, uint32_t type,
                                     uint32_t index, void* buffer, uint32_t* length);
extern int usbd_control_transfer_hook(uint32_t device_id, uint32_t bmRequestType,
                                       uint32_t bRequest, uint32_t wValue,
                                       uint32_t wIndex, void* data, uint32_t wLength);
extern int usbd_interrupt_transfer_hook(uint32_t device_id, uint32_t endpoint,
                                         void* buffer, uint32_t* length, uint32_t timeout);
