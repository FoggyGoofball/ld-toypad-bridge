/**
 * ldd_driver.h
 * Native Logical Device Driver interface for LEGO Dimensions Toy Pad
 *
 * Registers an Extra LDD (Low-Level Device Driver) with CellOS so that
 * when a USB device matching the Toy Pad VID/PID is physically connected,
 * the plugin claims it and bridges its HID endpoints to the network layer.
 *
 * Until a real device is attached, the background thread runs in
 * "network-only" mode — the server can detect the PS3 via UDP beacons,
 * but USB interception is deferred to when hardware appears.
 */

#ifndef LDD_DRIVER_H
#define LDD_DRIVER_H

#include <stdint.h>

/**
 * Maximum number of USB pipes (endpoints) we track.
 * Toy Pad uses exactly 2: one Interrupt IN (0x81), one Interrupt OUT (0x01).
 */
#define LDD_MAX_PIPES 4

/**
 * Opaque pipe handle returned by cellUsbdOpenPipe.
 */
typedef int ldd_pipe_handle_t;

/**
 * State tracked for a claimed Toy Pad device.
 */
typedef struct {
    int         claimed;         /**< 1 if a device is currently attached       */
    int         dev_index;       /**< Kernel-assigned device index              */
    uint8_t     ep_addr_in;      /**< IN endpoint address (e.g. 0x81)           */
    uint8_t     ep_addr_out;     /**< OUT endpoint address (e.g. 0x01)          */
    ldd_pipe_handle_t pipe_in;   /**< Open pipe handle for IN endpoint          */
    ldd_pipe_handle_t pipe_out;  /**< Open pipe handle for OUT endpoint         */
    uint8_t     raw_in[64];      /**< Last IN data received                     */
    int         raw_in_len;      /**< Length of last IN data                    */
} ldd_device_t;

/* LDD global state -- CRT bypass accessible from main.c */
struct ldd_global_state {
    int            registered;
    ldd_device_t   device;
};
extern struct ldd_global_state g_ldd;

/**
 * Initialize the Extra LDD subsystem.
 *
 * Registers a CellUsbdLddOps structure so the kernel notifies us
 * when a USB device with matching VID/PID is connected.
 *
 * Must be called *after* network_init() so that UDP is ready.
 *
 * @return 0 on success, negative on error (registration not supported).
 */
int ldd_driver_init(void);

/**
 * Shutdown the Extra LDD subsystem.
 *
 * Unregisters the driver and closes any open pipes.
 */
void ldd_driver_shutdown(void);

/**
 * Try to read a packet from the IN endpoint (non-blocking).
 *
 * If no device is currently claimed or no data is available,
 * returns 0 without blocking.
 *
 * @param data  Buffer to receive data (64 bytes max).
 * @param len   On return, set to number of bytes read.
 * @return 1 if data was read, 0 if no data, negative on error.
 */
int ldd_recv_in(uint8_t *data, int *len);

/**
 * Send a packet to the OUT endpoint (blocking, with timeout).
 *
 * @param data  Buffer to send.
 * @param len   Number of bytes to send.
 * @return bytes sent on success, negative on error.
 */
int ldd_send_out(const uint8_t *data, int len);

/**
 * Get current device attachment status.
 *
 * @return 1 if a Toy Pad device is currently claimed, 0 otherwise.
 */
int ldd_has_device(void);

#endif /* LDD_DRIVER_H */
