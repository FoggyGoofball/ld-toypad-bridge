/**
 * network.h
 * Network communication interface for the PS3 .sprx plugin
 *
 * Provides UDP send/receive primitives for communicating with the
 * LD-ToyPad server on the development PC.
 *
 * Protocol: UDP (low latency, connectionless)
 * Default port: 28472
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <ppu-types.h>

// Packet types sent from PS3 -> PC
#define NET_PACKET_TYPE_POLL       0x01  // USB interrupt poll request
#define NET_PACKET_TYPE_READ_TAG   0x02  // Read tag data from a zone
#define NET_PACKET_TYPE_WRITE_TAG  0x03  // Write data to a tag
#define NET_PACKET_TYPE_DATA_OUT   0x04  // USB interrupt OUT data

// Response status from PC -> PS3
#define NET_RESPONSE_OK            0x00  // Successful response with data
#define NET_RESPONSE_NO_TAG        0x01  // No tag on requested zone
#define NET_RESPONSE_ERROR         0xFF  // General error

// Packet size constants
#define NET_PACKET_HEADER_SIZE     8     // Minimum packet header size
#define NET_PACKET_MAX_SIZE        80    // Maximum packet size (matching USB HID)

/**
 * Initialize the network subsystem
 * Creates the UDP socket and binds to the configured port.
 * 
 * @param port UDP port to bind to
 * @return 0 on success, negative on error
 */
int network_init(uint16_t port);

/**
 * Shutdown the network subsystem
 * Closes the UDP socket and frees resources.
 */
void network_shutdown(void);

/**
 * Send a packet to the LD-ToyPad server
 * Non-blocking send. If server address hasn't been discovered yet,
 * returns an error.
 *
 * @param data Packet data buffer
 * @param len Length of data to send
 * @return 0 on success, negative on error
 */
int network_send(const uint8_t* data, int len);

/**
 * Receive a packet from the LD-ToyPad server
 * Non-blocking receive. Returns immediately if no data available.
 *
 * @param buffer Buffer to receive into
 * @param buf_size Size of the buffer
 * @return Number of bytes received, 0 if no data, negative on error
 */
int network_recv(uint8_t* buffer, int buf_size);

/**
 * Send a poll request to the server
 * This is called from the USB interrupt IN hook.
 *
 * @param zone Zone number (0=LEFT, 1=CENTER, 2=RIGHT)
 * @param sequence Sequence counter for packet matching
 * @return 0 on success, negative on error
 */
int network_send_poll(uint8_t zone, uint8_t sequence);

/**
 * Send tag data to the server (from USB interrupt OUT)
 *
 * @param zone Zone number
 * @param sequence Sequence counter
 * @param data Data from the USB OUT transfer
 * @param len Length of the data
 * @return 0 on success, negative on error
 */
int network_send_data(uint8_t zone, uint8_t sequence, const uint8_t* data, int len);

/**
 * Attempt server discovery when no server endpoint is known.
 * Sends a throttled UDP broadcast probe packet.
 *
 * @param sequence Sequence counter value used for probe packet
 */
void network_maybe_probe_server(uint8_t sequence);

/**
 * Set the PC server's IP address (optional, for when discovery isn't working)
 *
 * @param ip IP address in network byte order
 * @param port Port number in host byte order
 */
void network_set_server(uint32_t ip, uint16_t port);

/**
 * Get the current server address
 *
 * @return 1 if server address is known, 0 if not
 */
int network_has_server(void);

#endif // NETWORK_H
