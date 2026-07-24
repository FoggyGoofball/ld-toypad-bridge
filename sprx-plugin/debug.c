/**
 * debug.c — Debug logging for LD-ToyPad SPRX (Sony SDK)
 *
 * Provides:
 *   1. Ring buffer in memory (64KB)
 *   2. File logging to /dev_hdd0/plugins/ldtoypad_debug.log via cellFsWrite
 *   3. Remote UDP log streaming via BSD sockets (sendto)
 *
 * Sony SDK equivalents:
 *   cellFsOpen / cellFsWrite / cellFsClose  (from -lfs_stub)
 *   socket / sendto / close                  (from -lnet_stub)
 */

#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include <cell/cell_fs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "debug.h"

#define DEBUG_RING_BUFFER_SIZE  (64 * 1024)
#define DEBUG_MAX_LINE_LENGTH   256

/* g_debug is defined here (extern in debug.h) */
struct debug_state g_debug;

static void append_char(char* out, int cap, int* pos, char c)
{
    if (*pos + 1 >= cap) {
        return;
    }
    out[*pos] = c;
    (*pos)++;
}

static void append_str(char* out, int cap, int* pos, const char* s)
{
    if (s == NULL) {
        s = "(null)";
    }

    while (*s != '\0') {
        append_char(out, cap, pos, *s);
        s++;
    }
}

static void append_u64_base(char* out, int cap, int* pos, uint64_t v, int base, int uppercase)
{
    char tmp[32];
    int i = 0;
    const char* d = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (base < 2 || base > 16) {
        return;
    }

    if (v == 0) {
        append_char(out, cap, pos, '0');
        return;
    }

    while (v != 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = d[v % (uint64_t)base];
        v /= (uint64_t)base;
    }

    while (i > 0) {
        i--;
        append_char(out, cap, pos, tmp[i]);
    }
}

static int debug_vformat(char* out, int cap, const char* fmt, va_list ap)
{
    int pos = 0;

    if (fmt == NULL || cap <= 1) {
        return 0;
    }

    while (*fmt != '\0') {
        if (*fmt != '%') {
            append_char(out, cap, &pos, *fmt);
            fmt++;
            continue;
        }

        fmt++;
        if (*fmt == '\0') {
            break;
        }

        while (*fmt == '0' || *fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '.') {
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            fmt++;
        }

        int len_l = 0;
        while (*fmt == 'l') {
            len_l++;
            fmt++;
        }
        while (*fmt == 'h') {
            fmt++;
        }

        if (*fmt == '\0') {
            break;
        }

        switch (*fmt) {
            case '%':
                append_char(out, cap, &pos, '%');
                break;
            case 'c': {
                int v = va_arg(ap, int);
                append_char(out, cap, &pos, (char)v);
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                append_str(out, cap, &pos, s);
                break;
            }
            case 'd':
            case 'i': {
                int64_t v;
                if (len_l >= 2) {
                    v = (int64_t)va_arg(ap, long long);
                } else if (len_l == 1) {
                    v = (int64_t)va_arg(ap, long);
                } else {
                    v = (int64_t)va_arg(ap, int);
                }
                if (v < 0) {
                    uint64_t uv = (uint64_t)(-(v + 1)) + 1;
                    append_char(out, cap, &pos, '-');
                    append_u64_base(out, cap, &pos, uv, 10, 0);
                } else {
                    append_u64_base(out, cap, &pos, (uint64_t)v, 10, 0);
                }
                break;
            }
            case 'u': {
                uint64_t v;
                if (len_l >= 2) {
                    v = (uint64_t)va_arg(ap, unsigned long long);
                } else if (len_l == 1) {
                    v = (uint64_t)va_arg(ap, unsigned long);
                } else {
                    v = (uint64_t)va_arg(ap, unsigned int);
                }
                append_u64_base(out, cap, &pos, (uint64_t)v, 10, 0);
                break;
            }
            case 'x': {
                uint64_t v;
                if (len_l >= 2) {
                    v = (uint64_t)va_arg(ap, unsigned long long);
                } else if (len_l == 1) {
                    v = (uint64_t)va_arg(ap, unsigned long);
                } else {
                    v = (uint64_t)va_arg(ap, unsigned int);
                }
                append_u64_base(out, cap, &pos, (uint64_t)v, 16, 0);
                break;
            }
            case 'X': {
                uint64_t v;
                if (len_l >= 2) {
                    v = (uint64_t)va_arg(ap, unsigned long long);
                } else if (len_l == 1) {
                    v = (uint64_t)va_arg(ap, unsigned long);
                } else {
                    v = (uint64_t)va_arg(ap, unsigned int);
                }
                append_u64_base(out, cap, &pos, (uint64_t)v, 16, 1);
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(ap, void*);
                append_str(out, cap, &pos, "0x");
                append_u64_base(out, cap, &pos, (uint64_t)v, 16, 0);
                break;
            }
            default:
                append_char(out, cap, &pos, '%');
                append_char(out, cap, &pos, *fmt);
                break;
        }

        fmt++;
    }

    if (pos + 1 < cap) {
        out[pos] = '\0';
    } else {
        out[cap - 1] = '\0';
    }

    return pos;
}

static void debug_ring_write(const char* text, int len)
{
    int i;
    int fd;
    uint64_t written;

    if (text == NULL || len <= 0) {
        return;
    }

    for (i = 0; i < len; i++) {
        g_debug.ring_buffer[g_debug.write_pos++ % DEBUG_RING_BUFFER_SIZE] = text[i];
    }

    /* File I/O via Sony SDK cellFs API */
    if (cellFsOpen("/dev_hdd0/plugins/ldtoypad_debug.log",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
                   &fd, NULL, 0) == CELL_OK) {
        cellFsWrite(fd, text, (uint64_t)len, &written);
        cellFsClose(fd);
    }
}

static void debug_send_remote(const char* text, int len)
{
    if (!g_debug.remote_enabled || g_debug.socket_fd < 0 || text == NULL || len <= 0) {
        return;
    }

    (void)sendto(g_debug.socket_fd,
                 text,
                 (size_t)len,
                 0,
                 (const struct sockaddr*)&g_debug.remote_addr,
                 (socklen_t)sizeof(g_debug.remote_addr));
}

void debug_init(void)
{
    g_debug.write_pos = 0;
    g_debug.socket_fd = -1;
    g_debug.remote_enabled = 0;
    g_debug.remote_addr.sin_family = AF_INET;
    g_debug.remote_addr.sin_port = 0;
    g_debug.remote_addr.sin_addr.s_addr = 0;

    g_debug.socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    g_debug.initialized = 1;
}

void debug_shutdown(void)
{
    if (g_debug.socket_fd >= 0) {
        socketclose(g_debug.socket_fd);
        g_debug.socket_fd = -1;
    }

    g_debug.remote_enabled = 0;
    g_debug.initialized = 0;
}

void debug_printf(const char* fmt, ...)
{
    char line[DEBUG_MAX_LINE_LENGTH];
    int len;
    va_list ap;

    if (!g_debug.initialized) {
        return;
    }

    va_start(ap, fmt);
    len = debug_vformat(line, (int)sizeof(line), fmt, ap);
    va_end(ap);

    if (len <= 0) {
        return;
    }

    if (line[len - 1] != '\n' && len + 1 < (int)sizeof(line)) {
        line[len++] = '\n';
        line[len] = '\0';
    }

    debug_ring_write(line, len);
    debug_send_remote(line, len);
}

void debug_set_remote(uint32_t ip, uint16_t port)
{
    if (!g_debug.initialized) {
        return;
    }

    if (ip == 0 || port == 0) {
        g_debug.remote_enabled = 0;
        return;
    }

    g_debug.remote_addr.sin_family = AF_INET;
    g_debug.remote_addr.sin_port = htons(port);
    g_debug.remote_addr.sin_addr.s_addr = ip;
    g_debug.remote_enabled = 1;

    debug_printf("[LDTP] remote debug target set port=%u", (unsigned int)port);
}

/**
 * Write init progress marker to HDD papertrail file.
 *
 * Writes the current g_init_progress step number to
 * /dev_hdd0/tmp/ld_paper.txt. Each write overwrites the file
 * atomically (write to .tmp, rename). This is used by the Node.js
 * injector to detect where the SPRX crashed during initialization.
 *
 * This is called from the INIT_PROGRESS(x) macro in both main.c
 * and usb_hooks.c, at every critical init step boundary.
 *
 * If the file write fails (e.g., HDD problem), we silently ignore it.
 * The in-memory g_init_progress variable is the primary diagnostic;
 * this file is the fallback for when the SPRX crashes before it can
 * write the main IPC file.
 */
void debug_write_progress(void)
{
    int fd;
    uint64_t written;
    char buf[64];
    int len;
    uint32_t step;

    /* Read g_init_progress via extern */
    extern volatile uint32_t g_init_progress;
    step = g_init_progress;

    /* Format the step number into buf */
    {
        char tmp[12];
        int i = 0;
        uint32_t v = step;
        if (v == 0) {
            tmp[i++] = '0';
        } else {
            char rev[12];
            int j = 0;
            while (v != 0 && j < 11) {
                rev[j++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (j > 0) {
                tmp[i++] = rev[--j];
            }
        }
        tmp[i] = '\0';

        /* Write to buf */
        for (len = 0; tmp[len]; len++) {
            buf[len] = tmp[len];
        }
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    /* Write to .tmp file, then atomic rename */
    if (cellFsOpen("/dev_hdd0/tmp/ld_paper.tmp",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_OK) {
        return; /* Silently ignore write failures */
    }

    cellFsWrite(fd, buf, len, &written);

    {
        int64_t close_ret = cellFsClose(fd);
        (void)close_ret;
    }

    /* Atomic rename — avoids partial-read race with Node.js */
    cellFsRename("/dev_hdd0/tmp/ld_paper.tmp",
                 "/dev_hdd0/tmp/ld_paper.txt");
}

void debug_hex_dump(const char* label, const uint8_t* data, int len)

{
    int i;

    if (label == NULL || data == NULL || len <= 0) {
        return;
    }

    debug_printf("%s len=%d", label, len);
    for (i = 0; i < len; i++) {
        debug_printf("  [%u] %X", (unsigned int)i, (unsigned int)data[i]);
    }
}
