/**
 * main_diag.c — DIAGNOSTIC NO-OP SPRX
 *
 * Does ABSOLUTELY NOTHING except sleep 15s and write a file.
 * No OPD scan, no network, no USB, no memory access.
 *
 * Purpose: isolate whether the XMB crash is from our code
 * or from the SPRXPatcher + TrueAncestor pipeline.
 */
#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <cell/cell_fs.h>

SYS_MODULE_INFO(ldtoypad, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);

static void worker_thread(uint64_t arg)
{
    (void)arg;

    /* SLEEP FOR 15 SECONDS. DO ABSOLUTELY NOTHING ELSE. */
    sys_timer_usleep(15 * 1000 * 1000);

    int fd;
    uint64_t written;
    if (cellFsOpen("/dev_hdd0/plugins/diagnostic.txt",
                   CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) == 0) {
        cellFsWrite(fd, "SPRX SURVIVED BOOT!\n", 20, &written);
        cellFsClose(fd);
    }
    sys_ppu_thread_exit(0);
}

int module_start(size_t args, void *argp)
{
    (void)args; (void)argp;

    sys_ppu_thread_t tid;
    int ret = sys_ppu_thread_create(&tid, worker_thread,
                                     0, 1000, 4096,
                                     SYS_PPU_THREAD_CREATE_JOINABLE,
                                     "diag");
    if (ret != 0)
        return SYS_PRX_NO_RESIDENT;

    return 0;
}

int module_stop(void)
{
    return SYS_PRX_STOP_OK;
}
