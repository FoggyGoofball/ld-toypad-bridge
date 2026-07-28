# Expert Question — 2026-07-24: sys_ppu_thread_create Fails After PS3MAPI Injection

## Summary

We've confirmed via fresh papertrail files (deleted before each injection) that
`module_start` runs successfully, `sys_ppu_thread_create` returns `CELL_OK` (0),
but the created thread **never executes its first instruction**. Even a
bare-minimum thread (sets one variable, calls `sys_ppu_thread_exit`) never runs.

## Evidence

### Test 1: Full worker_thread
- `module_start` creates thread with priority 1000, 64KB stack
- `sys_ppu_thread_create` returns `CELL_OK`
- Paper trail shows step 10 (thread created), never step 20 (worker entered)
- Thread function: `INIT_PROGRESS(20)` is literally the first instruction

### Test 2: probe_thread (bare minimum)
```c
static void probe_thread(uint64_t arg) {
    (void)arg;
    extern volatile uint32_t g_init_progress;
    g_init_progress = 20;
    __asm__ __volatile__ ("dcbst 0, %0\n\tsync" :: "r"(&g_init_progress) : "memory");
    sys_ppu_thread_exit(0);
}
```
- Same result: `sys_ppu_thread_create` returns `CELL_OK`, step 10 reached,
  step 20 never reached
- This thread does NOTHING but set a volatile global and exit
- No filesystem calls, no network, no imports — pure memory write

### Test 3: Filesystem writes work from module_start
- `progress_file()` writes directly to `/dev_hdd0/tmp/ld_paper.txt` using
  `cellFsOpen(TRUNC) → cellFsWrite → cellFsClose`
- Files appear fresh after injection, confirming `module_start` runs
- Multiple sequential writes (steps 1→5→10) all succeed
- But between `progress_file(10)` and `progress_file(11)` there's only
  `g_init_progress = 11; dcbst; sync;` — this appears to crash/hang

### Environment
- PS3 CECH-2501A, Cobra CFW 4.91, webMAN MOD 1.47.48q
- Injection via: `GET /ps3mapi.ps3?MODULE%20LOAD%200x{PID}%20{path}`
- Game: LEGO Dimensions (BLES-02175), PID 0x1010200
- SPRX: Sony DUPLEX SDK 3.40, `ppu-lv2-gcc -mprx -std=gnu99 -O2`
- Links: `-llv2_stub -lfs_stub -lnet_stub -lusbd_stub`
- Signed: oscetool 0.9.2, type APP (type PRX not supported by this version)

## Hypothesis

**PS3MAPI MODULE LOAD injects the SPRX into the game process but
`module_start` runs in the context of a webMAN/PS3MAPI handler thread,
not the game's main thread. When `sys_ppu_thread_create` is called from
this context, the new thread is created in the handler's thread group.
When the PS3MAPI HTTP handler returns (after `module_start` returns 0),
the handler's thread group is torn down, killing our worker thread
before the LV2 scheduler ever dispatches it.**

Supporting evidence:
- `sys_ppu_thread_create` returns `CELL_OK` — the kernel accepts the
  creation request
- The thread entry point is never called — not even for a bare-minimum
  thread with no function calls
- PS3MAPI returns `{"code": 200}` quickly — `module_start` returns
  cleanly, but the handler cleanup kills our thread

Alternative hypothesis: the game process has LV2 policies restricting
thread creation from dynamically-loaded PRX modules.

## Questions

1. **Does PS3MAPI MODULE LOAD run module_start in a temporary handler
   thread that gets cleaned up after the HTTP response?** If so, threads
   created from module_start would be destroyed before they can run.

2. **What is the recommended way to spawn a persistent thread from a
   PS3MAPI-injected SPRX?** Is there a different LV2 syscall (e.g.,
   `sys_ppu_thread_create_ex` with different flags) that creates a
   thread in the game's process context rather than the handler's?

3. **Would running ALL initialization synchronously in module_start
   (debug, network, LDD registration, toypad state) and only spawning
   the UDP main loop thread work?** If we do file+network+LDD init in
   module_start (which can take several seconds), the UDP loop thread
   could be created last. But this thread would still face the same
   thread-group cleanup issue.

4. **Is there an alternative to thread creation?** For example:
   a) Register a callback/handler that the game's main loop invokes?
   b) Hook `sys_ppu_thread_create` itself to piggyback on game threads?
   c) Use PPU fibers instead of threads (if supported)?
   d) Use a kernel-level approach (LV2 syscall hook via Cobra payload)?

5. **Could the `dcbst` instruction on the PRX data page be causing a
   DSI?** Between `progress_file(10)` and `progress_file(11)`, the only
   code is `g_init_progress = 11; dcbst; sync;`. If the PRX's `.data`
   section is mapped with non-standard caching attributes, `dcbst`
   could fault. But this doesn't explain why the probe_thread never
   executes (it also uses `dcbst` but from the thread context).
