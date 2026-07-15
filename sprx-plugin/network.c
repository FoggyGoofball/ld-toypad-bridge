/**
 * network.c
 * UDP transport for Toy Pad bridge traffic.
 *
 * Refactored: the startup recv spin (busy-wait loop) has been removed.
 * Discovery now happens naturally via the background thread's
 * periodic polling loop.
 */

#include <string.h>

#include <sys/socket.h>
#include <sys/systime.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network.h"
#include "debug.h"

#ifndef LDTP_DEBUG_LOG_PORT
#define LDTP_DEBUG_LOG_PORT 28473
#endif

#ifndef LDTP_DISCOVERY_INTERVAL_USEC
#define LDTP_DISCOVERY_INTERVAL_USEC 250000ULL
#endif

static struct {
    int socket_fd;
    int initialized;
    int server_known;
    int broadcast_enabled;
    uint16_t port;
    u64 last_probe_usec;
    struct sockaddr_in server;
    struct sockaddr_in discovery_target;
} g_net = {
    .socket_fd = -1,
    .initialized = 0,
    .server_known = 0,
    .broadcast_enabled = 0,
    .port = 0,
    .last_probe_usec = 0,
};

int network_init(uint16_t port)
{
    struct sockaddr_in local_addr;

    if (g_net.initialized) {
        return 0;
    }

    g_net.socket_fd = sysNetSocket(AF_INET, SOCK_DGRAM, 0);
    if (g_net.socket_fd < 0) {
        DEBUG_ERROR("[NET] socket failed: %d\n", g_net.socket_fd);
        return -1;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (sysNetBind(g_net.socket_fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        DEBUG_ERROR("[NET] bind failed on port %u\n", (unsigned)port);
        sysNetClose(g_net.socket_fd);
        g_net.socket_fd = -1;
        return -1;
    }

    /* Keep broadcast probing path enabled */
    g_net.broadcast_enabled = 1;

    memset(&g_net.server, 0, sizeof(g_net.server));
    g_net.server.sin_family = AF_INET;
    g_net.server.sin_port = htons(port);

    memset(&g_net.discovery_target, 0, sizeof(g_net.discovery_target));
    g_net.discovery_target.sin_family = AF_INET;
    g_net.discovery_target.sin_port = htons(port);
    g_net.discovery_target.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    g_net.port = port;
    g_net.server_known = 0;
    g_net.last_probe_usec = 0;
    g_net.initialized = 1;

    debug_set_remote(0, 0);

    DEBUG_PRINT("[NET] UDP ready on port %u\n", (unsigned)port);

    /* Send rapid startup beacon salvo so the server can detect us.
     * We send 10 beacons with a 100ms pause between each.  This
     * dramatically improves the chance the PC server catches the
     * initial announcement when boot timing is tight. */
    {
        int i;
        uint8_t beacon[NET_PACKET_HEADER_SIZE];
        memset(beacon, 0, sizeof(beacon));
        beacon[0] = NET_PACKET_TYPE_POLL;
        beacon[1] = 1;  /* center zone */
        beacon[2] = 0;

        for (i = 0; i < 10; i++) {
            beacon[2] = (uint8_t)i;
            (void)sysNetSendto(g_net.socket_fd, beacon, (size_t)sizeof(beacon), 0,
                               (const struct sockaddr*)&g_net.discovery_target,
                               (socklen_t)sizeof(g_net.discovery_target));
            sysUsleep(100000); /* 100 ms */
        }
    }

    /* NOTE: The old startup recv spin (busy-wait loop) has been REMOVED.
     * Server discovery now happens naturally via the background thread's
     * periodic network_recv() calls in the main loop. */

    return 0;
}

void network_shutdown(void)
{
    if (g_net.socket_fd >= 0) {
        sysNetClose(g_net.socket_fd);
        g_net.socket_fd = -1;
    }

    g_net.initialized = 0;
    g_net.server_known = 0;
    g_net.broadcast_enabled = 0;
    g_net.last_probe_usec = 0;
    debug_set_remote(0, 0);

    DEBUG_PRINT("[NET] UDP shutdown\n");
}

int network_send(const uint8_t* data, int len)
{
    int ret;

    if (!g_net.initialized || g_net.socket_fd < 0 || !g_net.server_known) {
        return -1;
    }

    ret = sysNetSendto(g_net.socket_fd, data, (size_t)len, 0,
                       (const struct sockaddr*)&g_net.server,
                       (socklen_t)sizeof(g_net.server));
    if (ret < 0) {
        DEBUG_ERROR("[NET] sendto failed: %d\n", ret);
        return ret;
    }

    DEBUG_VERBOSE("[NET] TX len=%d\n", ret);

    return ret;
}

int network_recv(uint8_t* buffer, int buf_size)
{
    struct sockaddr_in from_addr;
    socklen_t from_len = (socklen_t)sizeof(from_addr);
    int ret;

    if (!g_net.initialized || g_net.socket_fd < 0) {
        return -1;
    }

    ret = sysNetRecvfrom(g_net.socket_fd, buffer, (size_t)buf_size, MSG_DONTWAIT,
                         (const struct sockaddr*)&from_addr, &from_len);
    if (ret > 0 && !g_net.server_known) {
        memcpy(&g_net.server, &from_addr, sizeof(from_addr));
        g_net.server_known = 1;
        debug_set_remote(from_addr.sin_addr.s_addr, LDTP_DEBUG_LOG_PORT);
        DEBUG_PRINT("[NET] Server learned on port %u\n", (unsigned)ntohs(from_addr.sin_port));
        DEBUG_PRINT("[NET] Remote debug stream target %u\n", (unsigned)LDTP_DEBUG_LOG_PORT);
    } else if (ret > 0) {
        DEBUG_VERBOSE("[NET] RX len=%d\n", ret);
    }

    return ret;
}

int network_send_poll(uint8_t zone, uint8_t sequence)
{
    uint8_t packet[NET_PACKET_HEADER_SIZE];
    memset(packet, 0, sizeof(packet));
    packet[0] = NET_PACKET_TYPE_POLL;
    packet[1] = zone;
    packet[2] = sequence;
    return network_send(packet, (int)sizeof(packet));
}

void network_maybe_probe_server(uint8_t sequence)
{
    uint8_t packet[NET_PACKET_HEADER_SIZE];
    u64 sec = 0;
    u64 nsec = 0;
    u64 now_usec;

    if (!g_net.initialized || g_net.socket_fd < 0 || g_net.server_known || !g_net.broadcast_enabled) {
        return;
    }

    if (sysGetCurrentTime(&sec, &nsec) != 0) {
        return;
    }

    now_usec = (sec * 1000000ULL) + (nsec / 1000ULL);
    if (g_net.last_probe_usec != 0 && (now_usec - g_net.last_probe_usec) < LDTP_DISCOVERY_INTERVAL_USEC) {
        return;
    }

    memset(packet, 0, sizeof(packet));
    packet[0] = NET_PACKET_TYPE_POLL;
    packet[1] = 1;  /* center zone */
    packet[2] = sequence;

    if (sysNetSendto(g_net.socket_fd, packet, (size_t)sizeof(packet), 0,
                     (const struct sockaddr*)&g_net.discovery_target,
                     (socklen_t)sizeof(g_net.discovery_target)) >= 0) {
        g_net.last_probe_usec = now_usec;
        DEBUG_VERBOSE("[NET] Discovery probe broadcast sent\n");
    }
}

int network_send_data(uint8_t zone, uint8_t sequence,
                      const uint8_t* data, int len)
{
    uint8_t packet[NET_PACKET_HEADER_SIZE + 64];
    int copy_len = len;

    memset(packet, 0, sizeof(packet));
    packet[0] = NET_PACKET_TYPE_DATA_OUT;
    packet[1] = zone;
    packet[2] = sequence;

    if (copy_len < 0) {
        copy_len = 0;
    }
    if (copy_len > (int)(sizeof(packet) - NET_PACKET_HEADER_SIZE)) {
        copy_len = (int)(sizeof(packet) - NET_PACKET_HEADER_SIZE);
    }

    if (data != NULL && copy_len > 0) {
        memcpy(packet + NET_PACKET_HEADER_SIZE, data, (size_t)copy_len);
    }

    return network_send(packet, NET_PACKET_HEADER_SIZE + copy_len);
}

void network_set_server(uint32_t ip, uint16_t port)
{
    memset(&g_net.server, 0, sizeof(g_net.server));
    g_net.server.sin_family = AF_INET;
    g_net.server.sin_port = htons(port);
    g_net.server.sin_addr.s_addr = ip;
    g_net.server_known = 1;

    debug_set_remote(ip, LDTP_DEBUG_LOG_PORT);
    DEBUG_PRINT("[NET] Server set manually on port %u\n", (unsigned)port);
}

int network_has_server(void)
{
    return g_net.server_known ? 1 : 0;
}
