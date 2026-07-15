/**
 * toypad_state.h
 * Toy Pad state machine and USB descriptor definitions
 *
 * Manages the emulated LEGO Dimensions Toy Pad state and provides
 * spoofed USB descriptors. Based on:
 *   - Berny23/LD-ToyPad-Emulator (protocol specs)
 *   - PS3 USB HID device descriptor standards
 */

#ifndef TOYPAD_STATE_H
#define TOYPAD_STATE_H

#include <ppu-types.h>

// Toy Pad USB identifiers (LEGO Dimensions Toy Pad by PDP/Logic3)
#define TOYPAD_VID  0x0E6F  // Logic3 / PDP
#define TOYPAD_PID  0x0241  // LEGO Dimensions Toy Pad

// USB descriptor types
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22

// Pad zones
#define TOYPAD_ZONE_LEFT    0
#define TOYPAD_ZONE_CENTER  1
#define TOYPAD_ZONE_RIGHT   2
#define TOYPAD_NUM_ZONES    3

// Tag states
#define TAG_STATE_EMPTY     0x00
#define TAG_STATE_PLACED    0x01
#define TAG_STATE_REMOVED   0x02
#define TAG_STATE_PRESENT   0x03

// HID report sizes
#define HID_REPORT_SIZE     8     // Standard HID report (poll response)
#define HID_TAG_DATA_SIZE   80    // Full tag data report

// Endpoint addresses (standard for HID devices)
#define EP_INTERRUPT_IN     0x81  // IN endpoint (device -> PS3)
#define EP_INTERRUPT_OUT    0x01  // OUT endpoint (PS3 -> device)

/**
 * Initialize the Toy Pad state machine
 * Sets up default state (all zones empty)
 */
void toypad_state_init(void);

/**
 * Deinitialize the Toy Pad state machine
 */
void toypad_state_deinit(void);

/**
 * Set the device ID for the emulated Toy Pad
 *
 * @param device_id USB device identifier
 */
void toypad_state_set_device_id(uint32_t device_id);

/**
 * Get a spoofed USB descriptor for the Toy Pad
 *
 * @param type Descriptor type (DEVICE, CONFIGURATION, STRING, etc.)
 * @param index Descriptor index
 * @param buffer Output buffer for descriptor data
 * @param length Output: length of descriptor data written
 * @return 0 on success, negative on error
 */
int toypad_state_get_descriptor(uint32_t type, uint32_t index,
                                 void* buffer, uint32_t* length);

/**
 * Handle a USB control transfer for the Toy Pad
 *
 * @param bmRequestType USB request type bitmap
 * @param bRequest USB request code
 * @param wValue Request value
 * @param wIndex Request index
 * @param data Data buffer
 * @param wLength Length of data
 * @return 0 on success, negative on error
 */
int toypad_state_control_transfer(uint32_t bmRequestType, uint32_t bRequest,
                                   uint32_t wValue, uint32_t wIndex,
                                   void* data, uint32_t wLength);

/**
 * Handle a USB interrupt IN transfer (PS3 polls device for data)
 * This is the main polling path. We forward to the PC server via UDP.
 *
 * @param endpoint Endpoint address (should be 0x81 for IN)
 * @param buffer Buffer to write response data to
 * @param length Input: max length, Output: actual data length
 * @param timeout Timeout in milliseconds
 * @return 0 on success, negative on error
 */
int toypad_state_handle_interrupt_in(uint32_t endpoint, void* buffer,
                                      uint32_t* length, uint32_t timeout);

/**
 * Handle a USB interrupt OUT transfer (PS3 sends data to device)
 * We forward this data to the PC server via UDP.
 *
 * @param endpoint Endpoint address (should be 0x01 for OUT)
 * @param buffer Data from PS3
 * @param length Length of data
 * @return 0 on success, negative on error
 */
int toypad_state_handle_interrupt_out(uint32_t endpoint, const void* buffer,
                                       uint32_t length);

#endif // TOYPAD_STATE_H
