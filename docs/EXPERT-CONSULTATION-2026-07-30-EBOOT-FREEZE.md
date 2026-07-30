# Expert Consultation — 2026-07-30: EBOOT Re-Signing Freeze

**Project:** LD-ToyPad Bridge — LEGO Dimensions ToyPad emulation via PS3MAPI
**PS3:** CECH-2501A, Evilnat 4.93 CFW (Cobra 8.5), webMAN MOD 1.47.48q
**Game:** LEGO Dimensions BLUS31473 
**Original EBOOT:** 28.5 MB (Sony-encrypted, compressed SELF)
**Patched EBOOT:** 12.3 MB (oscetool 0.9.2 -5 APP, uncompressed SELF)
**Patch:** 8 bytes changed in ELF (VID `0x0E6F→0x0781`, PID `0x0241→0x5581`)

---

## The Problem

After patching 8 bytes in the decrypted ELF and re-signing with `oscetool -5 APP`, the game freezes with a hard lock at the "A new version is available. Do you want to update?" screen. No button input works. This happens identically in **JB folder** and **ISO** formats. All other variables have been eliminated:

- ❌ Filesystem redirection: ISO format uses Cobra block-level emulation — same freeze
- ❌ Stale install data: deleted all `/dev_hdd0/game/BLUS31473*` folders — same freeze
- ❌ Cobra plugins: held L2 during boot to disable — same freeze
- ❌ makeps3iso: ISO structure verified intact, file-by-file comparison correct

**The only changed variable is the EBOOT.BIN itself.** The freeze must be caused by the re-signing process.

---

## Questions

### Q1: Is `oscetool -5 APP` the correct signing type for this game?

We used:
```
oscetool --self-type APP --compress-data FALSE EBOOT.elf EBOOT.BIN
```

But the original EBOOT is 28.5 MB and our re-signed one is 12.3 MB — a 16 MB shrinkage. The original uses Sony's proprietary SELF compression; oscetool produces uncompressed output. Does this size/format difference alone cause the freeze? Is there a `--compress-data` flag or alternative mode that produces a SELF closer to the original structure?

Alternatives to try:
- `-0` (fake/debug SELF) — bypasses all encryption
- `-1` (retail SELF) — uses retail keys
- Different oscetool version or `scetool` (another signing tool)

Which one has the highest probability of passing the game's integrity check?

### Q2: Can we patch the encrypted EBOOT directly — no decryption, no re-signing?

This is the nuclear option. If oscetool can't produce a working SELF, can we:

1. Find the 8 patched bytes in the **encrypted** EBOOT.BIN (not decrypted ELF)
2. Apply the inverse of the encryption XOR/cipher to those 8 bytes
3. Write the encrypted-equivalent of the patched bytes back to the original EBOOT.BIN

The original EBOOT structure (headers, signature, metadata) stays 100% intact. Only the 8 bytes carrying the VID/PID change, and they're re-encrypted to match the surrounding ciphertext.

Is this feasible for a PS3 NPDRM SELF? What cipher does the PS3 use for EBOOT encryption? Can we compute the encrypted equivalent of `38A00781` from the known plaintext/ciphertext pair at offset 0x286DF0?

### Q3: Can we patch the VID/PID in memory at runtime via the SPRX?

If we can't produce a working patched EBOOT at all, the SPRX runs inside the game's process and can write to the game's .text segment. The plan:

1. Boot the **original unmodified** ISO
2. Game loads, registers LDD with ToyPad VID/PID (we miss this, but the game is running)
3. Inject SPRX at T+60s
4. SPRX scans EBOOT .text in RAM for `38 A0 02 41` (li r4, 0x0E6F) and `63 44 00 00` (ori r4, r4, 0x0241)
5. Overwrites them with `38 A0 07 81` and `38 80 07 81`
6. Flushes icache with dcbst/sync/icbi/isync
7. NOW: the game's in-memory code has SanDisk VID/PID. Next USB hotplug → kernel matches

Does the PS3's MMU protect EBOOT .text as read-only? If so, can we use `sys_memory_get_page_attribute` / `sys_memory_set_page_attribute` to temporarily make it writable? Are these syscalls available from a SPRX?

### Q4: Could this be a PARAM.SFO / TROPDIR / LICDIR mismatch?

When the game boots from ISO, it reads PARAM.SFO for version info and compares it against the EBOOT. Our PARAM.SFO is the original from the disc (version 01.00). The update prompt says a newer version exists. Does the game compute a hash of EBOOT.BIN and compare it against PARAM.SFO's digest field? If PARAM.SFO has a digest that doesn't match our re-signed EBOOT, would that cause a hard lock rather than a graceful error?

Could the TROPDIR (trophy data) or LICDIR (license data) contain hashes of the original EBOOT that trigger an integrity failure?

### Q5: Is there a known-working alternative to oscetool for this specific game?

Has anyone in the PS3 modding community successfully patched a LEGO Dimensions (or other TT Games/Warner Bros late-era PS3 title) EBOOT? These games may use a newer SELF format or additional integrity checks. Are there known workarounds — specific oscetool flags, a different tool, or a pre-patched EBOOT floating around?

---

## What We've Already Tried

| Attempt | Result |
|---------|--------|
| oscetool -5 APP, JB folder | Freeze at update screen |
| oscetool -5 APP, ISO (makeps3iso) | Freeze at update screen |
| Delete install data + retry | Freeze at update screen |
| Hold L2 during boot (disable Cobra) | Freeze at update screen |

---

## Desired Outcome

Get the game past the update screen and into the "Connect ToyPad" prompt, so we can inject the SPRX and test the full USB hook chain. If the EBOOT patch approach is fundamentally broken, confirm the in-memory patching alternative is viable so we can pivot immediately.
