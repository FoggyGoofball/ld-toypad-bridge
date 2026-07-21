/**
 * network.c — UDP transport for Toy Pad bridge (Sony SDK)
 *
 * Uses BSD socket API (socket/bind/sendto/recvfrom/close) from
 * -lnet_stub  (Sony SDK network library).
 *
 * Discovered server IP/port is stored in g_net.server.  When no
 * server is known, periodic broadcast probes are sent on the
 * discovery_target (INADDR_BROADCAST).
 */

#include <string.h>
#include <stdint.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <sys/timer.h>
#include <sys/sys_time.h>
#include <netex/net.h>
#include <cell/cell_fs.h>

#include "network.h"
#include "debug.h"


#ifndef LDTP_DEBUG_LOG_PORT
#define LDTP_DEBUG_LOG_PORT 28473
#endif

#ifndef LDTP_DISCOVERY_INTERVAL_USEC
#define LDTP_DISCOVERY_INTERVAL_USEC 250000ULL
#endif

/* g_net is defined here (extern in network.h) */
struct net_state g_net;

int network_init(uint16_t port)
{
    struct sockaddr_in local_addr;
    int ret;

    if (g_net.initialized) {
        return 0;
    }

    DEBUG_PRINT("[NET] network_init(port=%u) ENTRY\n", (unsigned)port);

    /* Initialize the sys_net network stack before any socket() calls.
     * sys_net_initialize_network() is a convenience macro that allocates
     * 128 KB of static memory and calls sys_net_initialize_network_ex().
     * It is safe to call multiple times (no-op after first init). */
    ret = sys_net_initialize_network();
    if (ret != 0) {
        DEBUG_ERROR("[NET] sys_net_initialize_network failed: 0x%x\n", ret);
        /* Continue anyway — the socket() retry below will handle deferral */
    }

    /* 2-second boot stabilization delay — let network interfaces spin up.
     * During early boot, the routing tables may not be populated and socket()
     * returns CELL_NET_ERROR_NET_NOT_INITIALIZED (0x80010010) or
     * ENETDOWN (0x80010041). Waiting here reduces retry count in the loop. */
    sys_timer_usleep(2000000);

    /* Retry socket() + bind() up to 120 times (12 seconds total at 100ms intervals).
     *
     * CRITICAL: Do NOT use SO_REUSEADDR here. UDP is connectionless and does not
     * have a TIME_WAIT state. If bind() fails with EADDRINUSE, the port is held
     * open by an orphaned socket from a previous SPRX injection that never ran
     * module_stop. SO_REUSEADDR would force-bind, but incoming UDP packets would
     * be nondeterministically routed between the ghost SPRX and the new SPRX,
     * causing severe packet loss. The correct fix is to explicitly unload the
     * previous PRX via Node.js orchestrator before injecting the new build.
     *
     * During early VSH boot, sys_net may not be fully loaded yet, and the network
     * interface may not have an IP assigned.  socket() returns
     * CELL_NET_ERROR_NET_NOT_INITIALIZED (0x80010010) early on, and bind()
     * will fail if the interface isn't ready.  By retrying the whole sequence,
     * we converge on the window when everything is live. */
    {
        int attempt;
        int last_socket_err = 0;
        int last_bind_err = 0;
        for (attempt = 0; attempt < 120; attempt++) {
            /* Create socket — use IPPROTO_UDP (17) for clarity */
            g_net.socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (g_net.socket_fd < 0) {
                last_socket_err = g_net.socket_fd;
                DEBUG_PRINT("[NET] socket() failed attempt %d/%d: 0x%x\n",
                            attempt + 1, 120, last_socket_err);
                sys_timer_usleep(100000); /* 100ms */
                continue;
            }

            /* Bind to port — NO SO_REUSEADDR (see comment above) */
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_port = htons(port);
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

            int bind_ret = bind(g_net.socket_fd, (struct sockaddr*)&local_addr, sizeof(local_addr));
            if (bind_ret == 0) {
                /* Both socket and bind succeeded */
                break;
            }
            last_bind_err = bind_ret; /* FIXED: was g_net.socket_fd (stored fd, not error) */
            DEBUG_PRINT("[NET] bind() failed attempt %d/%d: %d\n",
                        attempt + 1, 120, bind_ret);
            socketclose(g_net.socket_fd);
            g_net.socket_fd = -1;
            sys_timer_usleep(100000); /* 100ms */
        }
        if (g_net.socket_fd < 0) {
            DEBUG_ERROR("[NET] socket+bind failed after %d attempts (socket err: 0x%x, bind err: %d)\n",
                        120, last_socket_err, last_bind_err);
            return -1;
        }
        DEBUG_PRINT("[NET] socket()+bind() succeeded on attempt %d\n", attempt + 1);
    }

    /* Enable broadcast for discovery probes */
    {
        int optval = 1;
        if (setsockopt(g_net.socket_fd, SOL_SOCKET, SO_BROADCAST,
                       (void*)&optval, sizeof(optval)) < 0) {
            DEBUG_ERROR("[NET] setsockopt SO_BROADCAST failed\n");
            /* Non-fatal: discovery may still work on local subnet */
        }
    }

    /* Set non-blocking mode on socket using SO_NBIO (CellOS SDK).
     * On CellOS -lnet_stub, MSG_DONTWAIT flag in recvfrom() is unreliable.
     * Setting SO_NBIO via setsockopt ensures the socket descriptor itself
     * is non-blocking. recvfrom then uses flags=0. */
    {
        int optval = 1;  /* SO_NBIO: 0 = blocking, 1 = non-blocking */
        if (setsockopt(g_net.socket_fd, SOL_SOCKET, SO_NBIO,
                       (void*)&optval, sizeof(optval)) < 0) {
            DEBUG_ERROR("[NET] setsockopt SO_NBIO failed\n");
        }
    }

    /* NOTE: SO_RCVTIMEO is NOT used here because CellOS SDK 3.40
     * lacks struct timeval / POSIX socket timeout support.
     *
     * NETWORK MUTEX AVOIDANCE is handled by SO_NBIO (non-blocking)
     * set above — recvfrom returns immediately with -1/EWOULDBLOCK
     * when no data is available. The calling thread yields control
     * back to the OS scheduler between poll cycles, leaving the
     * sceNet subsystem available for sceNpTrophy synchronization.
     *
     * This prevents the deadlock chain:
     *   Game waits for Trophy sync
     *     → Trophy sync waits for sceNet
     *       → sceNet blocked by SPRX recvfrom
     *           (prevented by non-blocking socket + polling loop) */

    /* Keep broadcast probing path enabled */
    g_net.broadcast_enabled = 1;


    memset(&g_net.server, 0, sizeof(g_net.server));
    g_net.server.sin_family = AF_INET;
    g_net.server.sin_port = htons(port);

    memset(&g_net.discovery_target, 0, sizeof(g_net.discovery_target));
    g_net.discovery_target.sin_family = AF_INET;
    g_net.discovery_target.sin_port = htons(port);
    /* Use subnet broadcast (192.168.0.255) instead of INADDR_BROADCAST (255.255.255.255).
     * The PS3's routing stack handles global broadcast poorly — packets are frequently
     * dropped internally. Subnet broadcast is more reliable on the same /24 subnet.
     * Inet address: 192.168.0.255 = 0xC0A800FF in network byte order. */
    g_net.discovery_target.sin_addr.s_addr = 0xC0A800FF;

    g_net.port = port;
    g_net.server_known = 0;
    g_net.last_probe_usec = 0;
    g_net.self_ip = 0;
    /* NOT initialized yet — network_wait_ready() will mark it after
     * confirming the interface is up (DHCP complete). This prevents
     * sendto/recvfrom races during early boot. */
    g_net.initialized = 0;

    debug_set_remote(0, 0);

    DEBUG_PRINT("[NET] Socket bound on port %u, waiting for interface...\n", (unsigned)port);

    return 0;
}

/* Wait for network interface to be ready for I/O.
 * Uses getsockname() to poll for IP address assignment, which is the
 * most reliable indicator that DHCP has completed and the routing table
 * is populated. This replaces the previous blind 3-second sleep with
 * an early-exit poll loop.
 *
 * NETWORK MUTEX AVOIDANCE:
 * The PS3's sceNpTrophy background process requires a clear, unblocked
 * pathway through the CellOS network subsystem (sceNet). If the worker
 * thread blocks on recvfrom during Trophy synchronization, it can
 * trigger a network mutex lock. By waiting for IP assignment before
 * marking g_net.initialized, we ensure the socket is never used while
 * the network stack is still coming up.
 *
 * NOTE: Real cellNetCtl would be ideal, but SDK 3.40 lacks it. */
void network_wait_ready(void)
{
    int i;

    if (g_net.initialized || g_net.socket_fd < 0) {
        return;
    }

    DEBUG_PRINT("[NET] Waiting for network interface (polling getsockname)...\n");

    {
        struct sockaddr_in self_addr;
        socklen_t self_len = sizeof(self_addr);
        int ip_acquired = 0;

        /* Poll for IP assignment — up to 5 seconds at 100ms intervals.
         * Early exit as soon as getsockname returns a real IP (not INADDR_ANY).
         * This avoids the previous blind 3-second sleep and minimizes the
         * window where sceNpTrophy could contend for the network subsystem. */
        for (i = 0; i < 50; i++) {  /* 50 * 100ms = 5 seconds max */
            memset(&self_addr, 0, sizeof(self_addr));
            self_len = sizeof(self_addr);
            if (getsockname(g_net.socket_fd,
                            (struct sockaddr*)&self_addr, &self_len) == 0 &&
                self_addr.sin_addr.s_addr != htonl(INADDR_ANY)) {
                ip_acquired = 1;
                break;
            }
            sys_timer_usleep(100000);  /* 100ms between polls */
        }

        if (ip_acquired) {
            g_net.self_ip = self_addr.sin_addr.s_addr;
            {
                int fd_p;
                uint64_t written_p;
                if (cellFsOpen("/dev_hdd0/tmp/ld_self_ip.txt",
                               CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                               &fd_p, NULL, 0) == CELL_OK) {
                    char ipmsg[32];
                    uint32_t ip = ntohl(g_net.self_ip);
                    int pos = 0;
                    uint8_t o0 = (ip >> 24) & 0xFF;
                    uint8_t o1 = (ip >> 16) & 0xFF;
                    uint8_t o2 = (ip >> 8) & 0xFF;
                    uint8_t o3 = ip & 0xFF;
                    const char *p = "SELF_IP=";
                    while (*p) ipmsg[pos++] = *p++;
                    if (o0 >= 100) { ipmsg[pos++] = '0' + (o0 / 100); o0 %= 100; }
                    if (o0 >= 10)  { ipmsg[pos++] = '0' + (o0 / 10);  o0 %= 10; }
                    ipmsg[pos++] = '0' + o0; ipmsg[pos++] = '.';
                    if (o1 >= 100) { ipmsg[pos++] = '0' + (o1 / 100); o1 %= 100; }
                    if (o1 >= 10)  { ipmsg[pos++] = '0' + (o1 / 10);  o1 %= 10; }
                    ipmsg[pos++] = '0' + o1; ipmsg[pos++] = '.';
                    if (o2 >= 100) { ipmsg[pos++] = '0' + (o2 / 100); o2 %= 100; }
                    if (o2 >= 10)  { ipmsg[pos++] = '0' + (o2 / 10);  o2 %= 10; }
                    ipmsg[pos++] = '0' + o2; ipmsg[pos++] = '.';
                    if (o3 >= 100) { ipmsg[pos++] = '0' + (o3 / 100); o3 %= 100; }
                    if (o3 >= 10)  { ipmsg[pos++] = '0' + (o3 / 10);  o3 %= 10; }
                    ipmsg[pos++] = '0' + o3;
                    ipmsg[pos++] = '\n';
                    ipmsg[pos] = '\0';
                    cellFsWrite(fd_p, ipmsg, pos, &written_p);
                    cellFsClose(fd_p);
                }
            }
            DEBUG_PRINT("[NET] Self IP acquired via getsockname (waited %d polls)\n", i + 1);
        } else {
            g_net.self_ip = 0;
            DEBUG_PRINT("[NET] Could not determine self IP after 50 polls\n");
            /* self_ip stays 0 — self-rejection will be disabled,
             * but discovery may still work via packet-type filtering */
        }
    }

    /* Fire rapid beacon salvo now that interface should be ready.
     * Uses NET_PACKET_TYPE_DISCOVERY (0xF0) to distinguish from
     * normal poll packets (0x01), avoiding protocol collision. */
    DEBUG_PRINT("[NET] Sending startup beacon salvo...\n");
    {
        uint8_t beacon[NET_PACKET_HEADER_SIZE];
        memset(beacon, 0, sizeof(beacon));
        beacon[0] = NET_PACKET_TYPE_DISCOVERY;
        beacon[1] = 1;
        beacon[2] = 0;

        for (i = 0; i < 10; i++) {
            beacon[2] = (uint8_t)i;
            (void)sendto(g_net.socket_fd, beacon,
                         (size_t)sizeof(beacon), 0,
                         (const struct sockaddr*)&g_net.discovery_target,
                         (socklen_t)sizeof(g_net.discovery_target));
            sys_timer_usleep(100000);

        }
    }

    g_net.initialized = 1;
    DEBUG_PRINT("[NET] UDP ready on port %u\n", (unsigned)g_net.port);
}

void network_shutdown(void)
{
    if (g_net.socket_fd >= 0) {
        socketclose(g_net.socket_fd);
        g_net.socket_fd = -1;
    }

    g_net.initialized = 0;
    g_net.server_known = 0;
    g_net.broadcast_enabled = 0;
    g_net.last_probe_usec = 0;
    debug_set_remote(0, 0);

    /* Finalize sys_net network stack to release resources.
     * This is safe to call even if sys_net_initialize_network() was
     * never called or failed. */
    sys_net_finalize_network();

    DEBUG_PRINT("[NET] UDP shutdown\n");
}

int network_send(const uint8_t* data, int len)
{
    int ret;

    if (!g_net.initialized || g_net.socket_fd < 0 || !g_net.server_known) {
        return -1;
    }

    ret = sendto(g_net.socket_fd, data, (size_t)len, 0,
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

    /* Socket is set SO_NBIO (non-blocking) at init time, so plain recvfrom returns
     * -1/EWOULDBLOCK immediately when no data is available.
     * Do NOT use MSG_DONTWAIT flag — it's unreliable on CellOS -lnet_stub. */
    ret = recvfrom(g_net.socket_fd, buffer, (size_t)buf_size, 0,
                   (struct sockaddr*)&from_addr, &from_len);
    if (ret > 0) {
        /* ── SELF-REJECTION ──────────────────────────────────────────
         * Ignore packets looped back from our own socket.  When the PS3
         * sends a broadcast to 192.168.0.255:28472, the kernel mirrors
         * it back to any local socket listening on port 28472.  Without
         * this check, the PS3 would lock g_net.server to its own IP
         * and discovery would stall permanently. */
        if (g_net.self_ip != 0 &&
            from_addr.sin_addr.s_addr == g_net.self_ip) {
            DEBUG_VERBOSE("[NET] Ignored self-looped packet\n");
            return 0;
        }

        /* ── PAPERTRAIL: dump sender IP + first bytes ───────────────
         * Proves the PS3 is actually receiving the server's beacons.
         * Throttled to first 10 packets to avoid filesystem churn. */
        {
            static int recv_log_count = 0;
            if (recv_log_count < 10) {
                int fd_r;
                uint64_t written_r;
                if (cellFsOpen("/dev_hdd0/tmp/ld_recv_papertrail.txt",
                               CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                               &fd_r, NULL, 0) == CELL_OK) {
                    char rmsg[80];
                    uint32_t sip_raw = ntohl(from_addr.sin_addr.s_addr);
                    int rp = 0;
                    /* Format each octet as proper multi-digit decimal */
                    uint8_t r0 = (sip_raw >> 24) & 0xFF;
                    uint8_t r1 = (sip_raw >> 16) & 0xFF;
                    uint8_t r2 = (sip_raw >> 8) & 0xFF;
                    uint8_t r3 = sip_raw & 0xFF;
                    const char *rp_prefix = "RECV from=";
                    while (*rp_prefix) rmsg[rp++] = *rp_prefix++;
                    if (r0 >= 100) { rmsg[rp++] = '0' + (r0 / 100); r0 %= 100; }
                    if (r0 >= 10)  { rmsg[rp++] = '0' + (r0 / 10);  r0 %= 10; }
                    rmsg[rp++] = '0' + r0; rmsg[rp++] = '.';
                    if (r1 >= 100) { rmsg[rp++] = '0' + (r1 / 100); r1 %= 100; }
                    if (r1 >= 10)  { rmsg[rp++] = '0' + (r1 / 10);  r1 %= 10; }
                    rmsg[rp++] = '0' + r1; rmsg[rp++] = '.';
                    if (r2 >= 100) { rmsg[rp++] = '0' + (r2 / 100); r2 %= 100; }
                    if (r2 >= 10)  { rmsg[rp++] = '0' + (r2 / 10);  r2 %= 10; }
                    rmsg[rp++] = '0' + r2; rmsg[rp++] = '.';
                    if (r3 >= 100) { rmsg[rp++] = '0' + (r3 / 100); r3 %= 100; }
                    if (r3 >= 10)  { rmsg[rp++] = '0' + (r3 / 10);  r3 %= 10; }
                    rmsg[rp++] = '0' + r3;
                    rmsg[rp++] = ':';
                    /* port */
                    uint16_t sport = ntohs(from_addr.sin_port);
                    if (sport >= 10000) { rmsg[rp++] = '0' + (sport / 10000); sport %= 10000; }
                    if (sport >= 1000)  { rmsg[rp++] = '0' + (sport / 1000);  sport %= 1000; }
                    if (sport >= 100)   { rmsg[rp++] = '0' + (sport / 100);   sport %= 100; }
                    if (sport >= 10)    { rmsg[rp++] = '0' + (sport / 10);    sport %= 10; }
                    rmsg[rp++] = '0' + sport;
                    const char *rp_type = " type=";
                    while (*rp_type) rmsg[rp++] = *rp_type++;
                    rmsg[rp++] = '0';
                    rmsg[rp++] = 'x';
                    uint8_t type0 = (uint8_t)buffer[0];
                    rmsg[rp++] = (type0 >> 4) <= 9 ? '0' + (type0 >> 4) : 'A' + ((type0 >> 4) - 10);
                    rmsg[rp++] = (type0 & 0x0F) <= 9 ? '0' + (type0 & 0x0F) : 'A' + ((type0 & 0x0F) - 10);
                    const char *rp_len = " len=";
                    while (*rp_len) rmsg[rp++] = *rp_len++;
                    int rlen = ret;
                    if (rlen >= 100) { rmsg[rp++] = '0' + (rlen / 100); rlen %= 100; }
                    if (rlen >= 10)  { rmsg[rp++] = '0' + (rlen / 10);  rlen %= 10; }
                    rmsg[rp++] = '0' + rlen;
                    rmsg[rp++] = '\n';
                    rmsg[rp] = '\0';
                    cellFsWrite(fd_r, rmsg, rp, &written_r);
                    cellFsClose(fd_r);
                    recv_log_count++;
                }
            }
        }

        /* ── DISCOVERY LOGIC ────────────────────────────────────────
         * Only accept the sender as the server if the packet type
         * matches NET_PACKET_TYPE_DISCOVERY (0xF0).  This prevents
         * random broadcast noise or collision from locking us to a
         * wrong address.  Once server_known is set, all packets are
         * accepted normally. */
        if (!g_net.server_known) {
            if (buffer[0] == NET_PACKET_TYPE_DISCOVERY) {
                memcpy(&g_net.server, &from_addr, sizeof(from_addr));
                g_net.server_known = 1;
                debug_set_remote(from_addr.sin_addr.s_addr, LDTP_DEBUG_LOG_PORT);
                DEBUG_PRINT("[NET] Server learned via discovery: %u.%u.%u.%u:%u\n",
                            (unsigned)((ntohl(from_addr.sin_addr.s_addr) >> 24) & 0xFF),
                            (unsigned)((ntohl(from_addr.sin_addr.s_addr) >> 16) & 0xFF),
                            (unsigned)((ntohl(from_addr.sin_addr.s_addr) >> 8) & 0xFF),
                            (unsigned)(ntohl(from_addr.sin_addr.s_addr) & 0xFF),
                            (unsigned)ntohs(from_addr.sin_port));
            } else {
                DEBUG_VERBOSE("[NET] Ignored non-discovery packet during server search (type=0x%02x)\n",
                              (unsigned)buffer[0]);
            }
        } else {
            DEBUG_VERBOSE("[NET] RX len=%d\n", ret);
        }
    }

    return ret;
}

int network_send_keepalive(void)
{
    uint8_t packet[NET_PACKET_HEADER_SIZE];
    static uint64_t last_ka_usec = 0;
    uint64_t now_usec;

    if (!g_net.initialized || g_net.socket_fd < 0 || !g_net.server_known) {
        return -1;
    }

    now_usec = sys_time_get_system_time();
    if (last_ka_usec != 0 && (now_usec - last_ka_usec) < NET_KEEPALIVE_INTERVAL_USEC) {
        return 0; /* Not yet time for next heartbeat */
    }

    memset(packet, 0, sizeof(packet));
    packet[0] = NET_PACKET_TYPE_KEEPALIVE;  /* 0xEE */
    packet[1] = 0;  /* reserved / state byte */
    packet[2] = 0;  /* reserved / sub-type */

    if (network_send(packet, (int)sizeof(packet)) > 0) {
        last_ka_usec = now_usec;
        DEBUG_VERBOSE("[NET] Keepalive heartbeat sent\n");
    }

    return 0;
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
    uint64_t now_usec;

    if (!g_net.initialized || g_net.socket_fd < 0 || g_net.server_known || !g_net.broadcast_enabled) {
        return;
    }

    now_usec = sys_time_get_system_time();
    if (g_net.last_probe_usec != 0 && (now_usec - g_net.last_probe_usec) < LDTP_DISCOVERY_INTERVAL_USEC) {
        return;
    }

    /* RAW PAPERTRAIL: Confirm probe fires by writing directly to HDD.
     * Throttled to first 5 calls to avoid choking VSH with disk I/O.
     * This bypasses all debug macros — it proves the function executes. */
    {
        static int probe_log_count = 0;
        if (probe_log_count < 5) {
            int fd;
            uint64_t written;
            if (cellFsOpen("/dev_hdd0/tmp/ld_probe_papertrail.txt",
                           CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                           &fd, NULL, 0) == CELL_OK) {
                char msg[64];
                /* Build message manually (no snprintf dependency) */
                const char *prefix = "PROBE seq=";
                int mi = 0;
                /* Write prefix */
                while (prefix[mi]) {
                    msg[mi] = prefix[mi];
                    mi++;
                }
                /* Write sequence digit (simple 0-9) */
                uint8_t s = sequence;
                if (s >= 100) { msg[mi++] = '0' + (s / 100); s %= 100; }
                if (s >= 10)  { msg[mi++] = '0' + (s / 10);  s %= 10; }
                msg[mi++] = '0' + s;
                msg[mi++] = '\n';
                msg[mi] = '\0';
                cellFsWrite(fd, msg, mi, &written);
                cellFsClose(fd);
                probe_log_count++;
            }
        }
    }

    memset(packet, 0, sizeof(packet));
    packet[0] = NET_PACKET_TYPE_DISCOVERY;
    packet[1] = 1;  /* center zone */
    packet[2] = sequence;

    if (sendto(g_net.socket_fd, packet, (size_t)sizeof(packet), 0,
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
