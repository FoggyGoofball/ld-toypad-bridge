/**
 * main.c — Hardware Diagnostic Buzz Stub for Evilnat 4.93 Cobra 8.5
 *
 * Minimal SPRX that rings the PS3 hardware buzzer via raw syscall 392.
 * Runs the syscall in a SEPARATE PPU THREAD (required by Cobra).
 * Uses COMPLETE clobber list for all registers destroyed by sc.
 *
 * Build: Sony SDK v3.40 with -mprx -fno-builtin -nostdlib
 */

#include <sys/prx.h>
#include <sys/ppu_thread.h>


SYS_MODULE_INFO(HelloWorld, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);


/* buzzer_thread — Beeps 4 times to confirm plugin loading */
void buzzer_thread(uint64_t arg)
{
    (void)arg;
    int i;

    for (i = 0; i < 4; i++)
    {
        /* Ring buzzer */
        __asm__ volatile(
            "li 11, 392\n\t"
            "li 3, 0x1004\n\t"
            "li 4, 0x4\n\t"
            "li 5, 0x6\n\t"
            "sc\n\t"
            :
            :
            : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
              "r11", "r12", "r0", "cr0", "ctr", "xer", "lr"
        );

        /* Delay loop using register-based counter */
        __asm__ volatile(
            "lis 8, 0x004C\n\t"
            "ori 8, 8, 0x4B40\n\t"
            "1:\n\t"
            "subic. 8, 8, 1\n\t"
            "bne 1b\n\t"
            :
            :
            : "r8", "cr0"
        );
    }

    sys_ppu_thread_exit(0);
}


int module_start(size_t args, void *argp)
{
    (void)args;
    (void)argp;

    sys_ppu_thread_t tid;
    sys_ppu_thread_create(
        &tid,
        buzzer_thread,
        0,
        0,
        0x1000,
        0,
        "buzzer"
    );

    return 0;
}


int module_stop(void)
{
    return SYS_PRX_STOP_OK;
}
