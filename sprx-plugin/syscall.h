/**
 * syscall.h ??? PPU syscall wrappers for Sony SDK SPRX builds
 *
 * Provides lightweight inline asm wrappers for LV2 syscalls that
 * are not provided by any Sony SDK stub library.  These use the
 * standard PPU `sc` instruction with full clobber lists.
 *
 * Available wrappers:
 *   sys_usleep(usec)          ??? sc 74:  microsecond sleep
 *   sys_get_timebase_usec()   ??? mftb:  return timebase in microseconds
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * sys_usleep ??? LV2 syscall 74
 *
 * Busy-wait sleep accurate to ~1 microsecond.
 * Clobbers: r3 (return), r11, r12, cr0, memory.
 *
 * @param usec  Microseconds to sleep (0 = yield timeslice)
 * @return      0 on success, negative on error
 */
static inline int sys_usleep(uint64_t usec)
{
    register uint64_t num __asm__("r3") = 74;
    register uint64_t arg1 __asm__("r4") = usec;
    __asm__ volatile(
        "sc\n"
        : "+r"(num)
        : "r"(arg1)
        : "r5", "r6", "r7", "r8", "r9", "r10",
          "r11", "r12", "r0", "cr0", "ctr", "xer", "lr",
          "memory"
    );

    return (int)num;
}

/**
 * sys_get_timebase_usec ??? Read PPU timebase in microseconds
 *
 * Uses the mftb (Move From Time Base) instruction to read the
 * 64-bit timebase register (TBR 268/269).  On Cell/BE this
 * increments at a known frequency tied to the system bus clock.
 * We divide by a hard-coded approximate divisor to get ??s.
 *
 * WARNING: This is an *estimate* (~+/-40ppm).  The exact divisor
 * depends on the actual PS3 model's timebase frequency.  For most
 * models it's 80000000 (80 MHz) dividing to ~12.5 ns ticks.  We
 * use 80 here for microsecond resolution (80 ticks ??? 1 ??s).
 *
 * @return  Approximate microseconds since boot (may wrap)
 */
static inline uint64_t sys_get_timebase_usec(void)
{
    uint32_t tbl, tbu;
    __asm__ volatile(
        "1: mftbu %0\n"
        "   mftb  %1\n"
        "   mftbu %2\n"
        "   cmpw  %0, %2\n"
        "   bne   1b\n"
        : "=r"(tbu), "=r"(tbl), "=r"(tbu)
        :
        : "cr0"
    );
    uint64_t tb = ((uint64_t)tbu << 32) | (uint64_t)tbl;
    /* Divide by timebase MHz to get microseconds.
     * PS3 timebase is typically 80 MHz (80000000 Hz).
     * 80 ticks = 1 ??s. */
    return tb / 80ULL;
}

#ifdef __cplusplus
}
#endif

#endif /* SYSCALL_H */
