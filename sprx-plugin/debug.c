#include <stdarg.h>
#include <ppu-types.h>
#include <sys/socket.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <lv2/sysfs.h>

#include "debug.h"

#define DEBUG_RING_BUFFER_SIZE  (64 * 1024)
#define DEBUG_MAX_LINE_LENGTH   256

struct debug_state {
    char ring_buffer[DEBUG_RING_BUFFER_SIZE];
    uint32_t write_pos;
    int socket_fd;
    int remote_enabled;
    struct sockaddr_in remote_addr;
    int initialized;
};
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

static void append_u64_base(char* out, int cap, int* pos, u64 v, int base, int uppercase)
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
        tmp[i++] = d[v % (u64)base];
        v /= (u64)base;
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
                s64 v;
                if (len_l >= 2) {
                    v = (s64)va_arg(ap, long long);
                } else if (len_l == 1) {
                    v = (s64)va_arg(ap, long);
                } else {
                    v = (s64)va_arg(ap, int);
                }
                if (v < 0) {
                    u64 uv = (u64)(-(v + 1)) + 1;
                    append_char(out, cap, &pos, '-');
                    append_u64_base(out, cap, &pos, uv, 10, 0);
                } else {
                    append_u64_base(out, cap, &pos, (u64)v, 10, 0);
                }
                break;
            }
            case 'u': {
                u64 v;
                if (len_l >= 2) {
                    v = (u64)va_arg(ap, unsigned long long);
                } else if (len_l == 1) {
                    v = (u64)va_arg(ap, unsigned long);
                } else {
                    v = (u64)va_arg(ap, unsigned int);
                }
                append_u64_base(out, cap, &pos, (u64)v, 10, 0);
                break;
            }
            case 'x': {
                u64 v;
                if (len_l >= 2) {
                    v = (u64)va_arg(ap, unsigned long long);
                } else if (len_l == 1) {
                    v = (u64)va_arg(ap, unsigned long);
                } else {
                    v = (u64)va_arg(ap, unsigned int);
                }
                append_u64_base(out, cap, &pos, (u64)v, 16, 0);
                break;
            }
            case 'X': {
                u64 v;
                if (len_l >= 2) {
                    v = (u64)va_arg(ap, unsigned long long);
                } else if (len_l == 1) {
                    v = (u64)va_arg(ap, unsigned long);
                } else {
                    v = (u64)va_arg(ap, unsigned int);
                }
                append_u64_base(out, cap, &pos, (u64)v, 16, 1);
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(ap, void*);
                append_str(out, cap, &pos, "0x");
                append_u64_base(out, cap, &pos, (u64)v, 16, 0);
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

    /* INJECTED: Physical File I/O Restoration */
    CellFsMode debug_log_mode = 0666;
    if (sysFsOpen("/dev_hdd0/plugins/ldtoypad_debug.log",
                  SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                  &fd, &debug_log_mode, sizeof(CellFsMode)) == 0) {
        sysFsWrite(fd, text, (uint64_t)len, &written);
        sysFsClose(fd);
    }
}

static void debug_send_remote(const char* text, int len)
{
    if (!g_debug.remote_enabled || g_debug.socket_fd < 0 || text == NULL || len <= 0) {
        return;
    }

    (void)sysNetSendto(g_debug.socket_fd,
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

    g_debug.socket_fd = sysNetSocket(AF_INET, SOCK_DGRAM, 0);
    g_debug.initialized = 1;
}

void debug_shutdown(void)
{
    if (g_debug.socket_fd >= 0) {
        sysNetClose(g_debug.socket_fd);
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
