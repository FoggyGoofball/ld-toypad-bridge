# Expert Question — 2026-07-27: Runtime NID Scanner Stuck

## Status
All previous recommendations implemented (done_cb, ControlTransfer hook, non-blocking socket). Boot log confirms new code runs. Game boots, reaches "Connect ToyPad." But the NID scanner consistently finds **0/5 hooks** across four different scanner architectures.

## What We've Tried

| Attempt | Scanner | Result |
|---------|---------|--------|
| 1 | 3-word triplet stride (`i+=3`) | 0/5 — format wrong |
| 2 | 1-word stride + forward GOT search (256 words) | 0/5 — GUID from wrong function? |
| 3 | String-based: find `"cellUsbd"` → follow `libname_ptr` | String not found at runtime (stripped) |
| 4 | Structsize-based: scan for `0x2C` byte → validate NID table contains known NIDs | **Just deployed, untested** |

## Key Finding
`analyze_eboot.py` confirms `cellUsbdControlTransfer` NID (`0x3219460D`) IS in the EBOOT at offset `0x007AC8DC`. But our runtime scanner (`0x00100000-0x01000000`) fails to find ANY of the 6 cellUsbd NIDs — including the one that's definitely present in the file.

## Question
Is approach #4 (structsize byte scan) the correct path, or is there a fundamental issue we're missing? Could the PS3 runtime memory layout differ from the ELF file layout in a way that makes NIDs inaccessible at the addresses we expect? Should we be scanning a different address range?

--- 
Full context: `docs/EXPERT-BRIEFING-2026-07-27-PRE-TEST.md`
