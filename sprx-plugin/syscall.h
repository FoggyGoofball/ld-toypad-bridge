/**
 * syscall.h -- PPU syscall wrappers for Sony SDK SPRX builds
 *
 * CRITICAL PS3 LV2 CONVENTION: r11 = syscall number, r3 = arg1, r4 = arg2, ...
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** sys_usleep: LV2 syscall 74, r11=74, r3=usec */
static inline int sys_usleep(uint64_t usec)
{
    int ret;
    __asm__ volatile(
        "li %%r11, 74\n\t"
        "mr %%r3, %1\n\t"
        "sc\n\t"
        "mr %0, %%r3\n"
        : "=r"(ret)
        : "r"(usec)
        : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
          "r11", "r12", "r0", "cr0", "cc", "ctr", "xer", "lr",
          "memory"
    );
    return ret;
}

/** sys_get_timebase_usec: mftb with atomic rollover check */
static inline uint64_t sys_get_timebase_usec(void)
{
    uint32_t tbu_hi, tbl, tbu_lo;
    __asm__ volatile(
        "1: mftbu %0\n"
        "   mftb  %1\n"
        "   mftbu %2\n"
        "   cmpw  %0, %2\n"
        "   bne   1b\n"
        : "=r"(tbu_hi), "=r"(tbl), "=r"(tbu_lo)
        :
        : "cr0"
    );
    uint64_t tb = ((uint64_t)tbu_hi << 32) | (uint64_t)tbl;
    return tb / 80ULL;
}

#ifdef __cplusplus
}
#endif
#endif
