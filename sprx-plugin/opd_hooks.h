/**
 * opd_hooks.h — OPD Overwrite Hooking for LEGO Dimensions
 *
 * Uses the "Self-Resolution Method": extracts the resolved code address
 * of cellUsbdInterruptTransfer from our own SPRX imports, scans the
 * game's memory for that address (the game's OPD entry), then overwrites
 * the game's OPD to point to our hook function.
 *
 * No assembly patching. No PLT needed. Pure data overwrite.
 * Expert-recommended approach for PPC64 ELFv1 OPD-based games.
 */

#ifndef OPD_HOOKS_H
#define OPD_HOOKS_H

#include <stdint.h>
#include <cell/usbd.h>

/* OPD structure on PS3 (32-bit ABI): 8 bytes total */
typedef struct {
    uint32_t func_addr;  /* code address */
    uint32_t toc_addr;   /* Table of Contents pointer */
} opd_t;

/* Hook our cellUsbdInterruptTransfer — same signature as the real API */
int32_t hook_cellUsbdInterruptTransfer(int32_t pipe_id, void *buf,
                                        int32_t len,
                                        CellUsbdDoneCallback done_cb,
                                        void *arg);

/* Initialize OPD hooks — scan game memory and install overwrites.
 * Returns 0 on success, -1 on failure. */
int opd_hooks_init(void);

/* Restore original OPD values. */
void opd_hooks_shutdown(void);

/* Check if hooks are active */
int opd_hooks_are_active(void);

/* Check if game LDD callbacks have been stolen */
int opd_hooks_has_game_ops(void);

/* Fire the game's probe() and attach() with fake ToyPad.
 * Returns 0 on success, -1 if callbacks not stolen or probe rejected. */
int opd_hooks_fire_hotplug(void);

/* Bridge API */
int opd_hooks_get_out_data(uint8_t *out, int max_len);
void opd_hooks_push_in_data(const uint8_t *data, int len);

#endif /* OPD_HOOKS_H */
