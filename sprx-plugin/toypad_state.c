/**
 * toypad_state.c
 * Toy Pad state machine and USB descriptor spoofing
 *
 * Implements the LEGO Dimensions Toy Pad emulation state machine.
 * Provides spoofed USB descriptors and handles interrupt transfers
 * by routing them over the network to the PC server.
 *
 * Based on Berny23/LD-ToyPad-Emulator protocol specifications.
 */

#include <string.h>
#include <stdlib.h>

#include <sys/usbd.h>

#include "toypad_state.h"
#include "network.h"
#include "debug.h"

// =========================================
// USB Device Descriptor for LEGO Toy Pad
// =========================================

/**
 * Standard USB Device Descriptor
 * Spoofed to match the LEGO Dimensions Toy Pad
 */
static const uint8_t g_device_descriptor[] = {
    0x12,                       // bLength: 18 bytes
    0x01,                       // bDescriptorType: DEVICE
    0x00, 0x02,                 // bcdUSB: USB 2.0
    0x00,                       // bDeviceClass: (defined at interface level)
    0x00,                       // bDeviceSubClass
    0x00,                       // bDeviceProtocol
    0x08,                       // bMaxPacketSize0: 8 bytes (control)
    (TOYPAD_VID >> 0) & 0xFF,   // idVendor (low byte)
    (TOYPAD_VID >> 8) & 0xFF,   // idVendor (high byte) - 0x0E6F = Logic3/PDP
    (TOYPAD_PID >> 0) & 0xFF,   // idProduct (low byte)
    (TOYPAD_PID >> 8) & 0xFF,   // idProduct (high byte) - 0x0241 = LEGO Dimensions
    0x00, 0x01,                 // bcdDevice: 1.00
    0x01,                       // iManufacturer: String index 1
    0x02,                       // iProduct: String index 2
    0x00,                       // iSerialNumber: No serial
    0x01                        // bNumConfigurations: 1
};

/**
 * USB Configuration Descriptor
 * Describes HID interface with interrupt endpoints
 */
static const uint8_t g_config_descriptor[] = {
    // Configuration descriptor
    0x09,                       // bLength
    0x02,                       // bDescriptorType: CONFIGURATION
    (0x09 + 0x09 + 0x09 + 0x07 + 0x07) & 0xFF,  // wTotalLength (low)
    (0x09 + 0x09 + 0x09 + 0x07 + 0x07) >> 8,    // wTotalLength (high) = 41 bytes
    0x01,                       // bNumInterfaces: 1
    0x01,                       // bConfigurationValue: 1
    0x00,                       // iConfiguration: no string
    0x80,                       // bmAttributes: Bus Powered
    0x32,                       // bMaxPower: 100mA

    // Interface descriptor (HID)
    0x09,                       // bLength
    0x04,                       // bDescriptorType: INTERFACE
    0x00,                       // bInterfaceNumber: 0
    0x00,                       // bAlternateSetting: 0
    0x02,                       // bNumEndpoints: 2 (IN + OUT)
    0x03,                       // bInterfaceClass: HID
    0x00,                       // bInterfaceSubClass: None
    0x00,                       // bInterfaceProtocol: None
    0x00,                       // iInterface: no string

    // HID descriptor
    0x09,                       // bLength
    0x21,                       // bDescriptorType: HID
    0x11, 0x01,                 // bcdHID: HID 1.11
    0x00,                       // bCountryCode: None
    0x01,                       // bNumDescriptors: 1
    0x22,                       // bDescriptorType: HID Report
    (0x3F) & 0xFF,              // wDescriptorLength (low)
    (0x3F) >> 8,                // wDescriptorLength (high) = 63 bytes

    // Endpoint descriptor (IN - device to host)
    0x07,                       // bLength
    0x05,                       // bDescriptorType: ENDPOINT
    0x81,                       // bEndpointAddress: IN endpoint 1
    0x03,                       // bmAttributes: Interrupt
    0x40, 0x00,                 // wMaxPacketSize: 64 bytes
    0x01,                       // bInterval: 1ms

    // Endpoint descriptor (OUT - host to device)
    0x07,                       // bLength
    0x05,                       // bDescriptorType: ENDPOINT
    0x01,                       // bEndpointAddress: OUT endpoint 1
    0x03,                       // bmAttributes: Interrupt
    0x40, 0x00,                 // wMaxPacketSize: 64 bytes
    0x01,                       // bInterval: 1ms
};

/**
 * HID Report Descriptor
 * Describes the Toy Pad's input/output reports
 */
static const uint8_t g_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,           // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,                 // Usage (Vendor Usage 1)
    0xA1, 0x01,                 // Collection (Application)
    
    // Input report (Toy Pad -> PS3)
    0x09, 0x02,                 // Usage (Vendor Usage 2)
    0x15, 0x00,                 // Logical Minimum (0)
    0x26, 0xFF, 0x00,           // Logical Maximum (255)
    0x75, 0x08,                 // Report Size (8)
    0x95, 0x50,                 // Report Count (80) - 80 byte input report
    0x81, 0x02,                 // Input (Data,Var,Abs)
    
    // Output report (PS3 -> Toy Pad)
    0x09, 0x03,                 // Usage (Vendor Usage 3)
    0x75, 0x08,                 // Report Size (8)
    0x95, 0x08,                 // Report Count (8) - 8 byte output report
    0x91, 0x02,                 // Output (Data,Var,Abs)
    
    0xC0                        // End Collection
};

/**
 * String descriptors
 */
static const uint8_t g_string_descriptor_0[] = {
    0x04, 0x03, 0x09, 0x04     // Language: English (0x0409)
};

static const uint8_t g_string_descriptor_1[] = {
    // "PDP" (Manufacturer)
    0x08, 0x03, 'P', 0x00, 'D', 0x00, 'P', 0x00
};

static const uint8_t g_string_descriptor_2[] = {
    // "LEGO Dimensions Toy Pad" (Product)
    0x30, 0x03,
    'L',0x00, 'E',0x00, 'G',0x00, 'O',0x00, ' ',0x00,
    'D',0x00, 'i',0x00, 'm',0x00, 'e',0x00, 'n',0x00,
    's',0x00, 'i',0x00, 'o',0x00, 'n',0x00, 's',0x00,
    ' ',0x00, 'T',0x00, 'o',0x00, 'y',0x00, ' ',0x00,
    'P',0x00, 'a',0x00, 'd',0x00
};

// =========================================
// Toy Pad State
// =========================================

typedef struct {
    uint32_t device_id;
    int active;
    int zones[TOYPAD_NUM_ZONES];  // Current tag state per zone
    uint32_t network_seq;         // Sequence counter for network packets
} toypad_state_t;

static toypad_state_t g_toypad = {0};

// =========================================
// Implementation
// =========================================

void toypad_state_init(void)
{
    DEBUG_PRINT("[TP] Toy Pad state initialized\n");
    memset(&g_toypad, 0, sizeof(g_toypad));
    g_toypad.active = 1;
    g_toypad.network_seq = 0;
}

void toypad_state_deinit(void)
{
    DEBUG_PRINT("[TP] Toy Pad state deinitialized\n");
    g_toypad.active = 0;
}

void toypad_state_set_device_id(uint32_t device_id)
{
    g_toypad.device_id = device_id;
    DEBUG_PRINT("[TP] Device ID set: 0x%08X\n", device_id);
}

int toypad_state_get_descriptor(uint32_t type, uint32_t index,
                                 void* buffer, uint32_t* length)
{
    DEBUG_VERBOSE("[TP] Get descriptor: type=%d, index=%d\n", type, index);

    switch (type) {
        case USB_DESC_DEVICE: {
            *length = sizeof(g_device_descriptor);
            memcpy(buffer, g_device_descriptor, *length);
            DEBUG_PRINT("[TP] Returned DEVICE descriptor (VID=0x%04X, PID=0x%04X)\n",
                       TOYPAD_VID, TOYPAD_PID);
            return 0;
        }

        case USB_DESC_CONFIGURATION: {
            *length = sizeof(g_config_descriptor);
            memcpy(buffer, g_config_descriptor, *length);
            DEBUG_PRINT("[TP] Returned CONFIGURATION descriptor (%d bytes)\n", *length);
            return 0;
        }

        case USB_DESC_STRING: {
            switch (index) {
                case 0:
                    *length = sizeof(g_string_descriptor_0);
                    memcpy(buffer, g_string_descriptor_0, *length);
                    break;
                case 1:
                    *length = sizeof(g_string_descriptor_1);
                    memcpy(buffer, g_string_descriptor_1, *length);
                    break;
                case 2:
                    *length = sizeof(g_string_descriptor_2);
                    memcpy(buffer, g_string_descriptor_2, *length);
                    break;
                default:
                    DEBUG_VERBOSE("[TP] Unknown string index: %d\n", index);
                    *length = 0;
                    return -1;
            }
            return 0;
        }

        case USB_DESC_HID: {
            // Return HID descriptor from config descriptor
            memcpy(buffer, g_config_descriptor + 18, 9);
            *length = 9;
            return 0;
        }

        case USB_DESC_HID_REPORT: {
            *length = sizeof(g_hid_report_descriptor);
            memcpy(buffer, g_hid_report_descriptor, *length);
            DEBUG_PRINT("[TP] Returned HID REPORT descriptor (%d bytes)\n", *length);
            return 0;
        }

        default:
            DEBUG_VERBOSE("[TP] Unhandled descriptor type: %d\n", type);
            *length = 0;
            return -1;
    }
}

int toypad_state_control_transfer(uint32_t bmRequestType, uint32_t bRequest,
                                   uint32_t wValue, uint32_t wIndex,
                                   void* data, uint32_t wLength)
{
    DEBUG_VERBOSE("[TP] Control: bmReqType=0x%02X, bReq=0x%02X, wVal=0x%04X, wIdx=0x%04X\n",
                 bmRequestType, bRequest, wValue, wIndex);

    // Standard USB control requests for HID devices
    uint8_t req_type = bmRequestType & 0x60; // 0x00=Standard, 0x20=Class, 0x40=Vendor
    uint8_t dir_in = bmRequestType & 0x80;   // 0x80 = device-to-host

    if (req_type == 0x00) {
        // Standard request
        switch (bRequest) {
            case 0x00: { // GET_STATUS
                if (dir_in) {
                    uint16_t status = 0x0000; // Device OK
                    memcpy(data, &status, 2);
                    return 2;
                }
                break;
            }
            case 0x05: { // SET_ADDRESS
                // Device address assignment - handled by USB core
                DEBUG_VERBOSE("[TP] Set address: %d\n", wValue);
                return 0;
            }
            case 0x06: { // GET_DESCRIPTOR
                return toypad_state_get_descriptor(wValue >> 8, wValue & 0xFF,
                                                   data, &wLength);
            }
            case 0x09: { // SET_CONFIGURATION
                DEBUG_PRINT("[TP] Set configuration: %d\n", wValue);
                return 0;
            }
        }
    } else if (req_type == 0x20) {
        // Class-specific request (HID)
        switch (bRequest) {
            case 0x01: { // GET_REPORT (HID)
                DEBUG_VERBOSE("[TP] HID Get Report: type=%d, id=%d\n",
                             wValue >> 8, wValue & 0xFF);
                // Return current pad state
                memset(data, 0, HID_REPORT_SIZE);
                return HID_REPORT_SIZE;
            }
            case 0x09: { // SET_REPORT (HID)
                DEBUG_VERBOSE("[TP] HID Set Report: type=%d, id=%d\n",
                             wValue >> 8, wValue & 0xFF);
                return wLength;
            }
            case 0x0A: { // SET_IDLE (HID)
                DEBUG_VERBOSE("[TP] HID Set Idle: duration=%d, id=%d\n",
                             wValue >> 8, wValue & 0xFF);
                return 0;
            }
            case 0x0B: { // SET_PROTOCOL (HID)
                DEBUG_VERBOSE("[TP] HID Set Protocol: %d\n", wValue);
                return 0;
            }
        }
    }

    // Unhandled control request - return stall
    DEBUG_VERBOSE("[TP] Unhandled control: 0x%02X, 0x%02X\n", bmRequestType, bRequest);
    return -1;
}

int toypad_state_handle_interrupt_in(uint32_t endpoint, void* buffer,
                                      uint32_t* length, uint32_t timeout)
{
    uint8_t seq = (uint8_t)(g_toypad.network_seq++);
    uint8_t response[NET_PACKET_MAX_SIZE];

    // Send poll request to PC server
    int ret = network_send_poll(TOYPAD_ZONE_CENTER, seq);
    if (ret < 0 && !network_has_server()) {
        // Attempt server discovery without blocking the USB poll path.
        network_maybe_probe_server(seq);
    }

    // Wait for response from server (non-blocking)
    // The USB interrupt handler will be called again next cycle
    int recv_len = network_recv(response, sizeof(response));
    
    if (recv_len > 2) {
        // Valid response received
        // Copy response data (skip header: status, zone, seq)
        uint8_t status = response[0];
        uint8_t resp_zone = response[1];
        
        if (status == NET_RESPONSE_OK) {
            int payload_len = recv_len - 3;
            if (payload_len > (int)*length) {
                payload_len = *length;
            }
            memcpy(buffer, response + 3, payload_len);
            *length = payload_len;
        } else {
            // No tag - return empty state
            memset(buffer, 0, *length);
            ((uint8_t*)buffer)[0] = 0x01;
        }
    } else {
        // No response yet - return current state
        // This is the normal case for most poll cycles
        if (!network_has_server()) {
            DEBUG_VERY_VERBOSE("[TP] No server yet, returning empty poll\n");
        }
        memset(buffer, 0, *length);
        ((uint8_t*)buffer)[0] = 0x01; // Report ID
    }

    return 0;
}

int toypad_state_handle_interrupt_out(uint32_t endpoint, const void* buffer,
                                       uint32_t length)
{
    uint8_t seq = (uint8_t)(g_toypad.network_seq++);

    DEBUG_PACKET("[TP] Interrupt OUT: ep=0x%02X, len=%d\n", endpoint, length);
    DEBUG_HEX_DUMP("[TP] OUT data", buffer, length);

    // Forward the data from PS3 to the PC server
    // This might contain tag write requests, LED commands, etc.
    network_send_data(TOYPAD_ZONE_CENTER, seq, buffer, length);

    return 0;
}
