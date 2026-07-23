# Build script for LD-ToyPad Bridge SPRX
# Copies sources to temp dir (no spaces), compiles, links, copies back
#
# REFACTORED 2026-07-22:
#   - Removed toc_trampoline.s, toc_trampoline_c.c (assembly wrappers no longer needed)
#   - Added trampoline_gen.c (dynamic PowerPC trampoline generator)
#   - Assembly-free: all TOC management done at runtime via trampoline_gen.c

$Tmp = "c:\temp\ldtoypad-src"
$CC = "c:\usr\local\cell\host-win32\ppu\ppu-lv2\bin\gcc.exe"
$SrcRoot = "c:\Users\Admin\source\repos\dimensions plugin\sprx-plugin"

# Define flags as array so PowerShell passes each flag as separate argument
$CFLAGS = @(
    "-mprx", "-std=gnu99", "-O2", "-g",
    "-fno-builtin", "-nodefaultlibs",
    "-Ic:\usr\local\cell\target\ppu\include"
)

$LDFLAGS = @(
    "-mprx", "-nodefaultlibs",
    "-llv2_stub", "-lfs_stub", "-lnet_stub", "-lusbd_stub"
)

# Clean and prepare temp directory
Remove-Item -Recurse -Force "$Tmp" -ErrorAction SilentlyContinue | Out-Null
New-Item -ItemType Directory -Force -Path "$Tmp\obj" | Out-Null
New-Item -ItemType Directory -Force -Path "$Tmp\build" | Out-Null

# Copy all required source files
$cSrcFiles = @("main.c","compat.c","network.c","debug.c","toypad_state.c","usb_hooks.c","trampoline_gen.c")
foreach ($f in $cSrcFiles) {
    Copy-Item "$SrcRoot\$f" "$Tmp\$f" -Force
}
$hdrFiles = @("network.h","debug.h","toypad_state.h","usb_hooks.h","trampoline_gen.h")
foreach ($f in $hdrFiles) {
    Copy-Item "$SrcRoot\$f" "$Tmp\$f" -Force
}

Write-Host "=== Compiling C sources ==="
foreach ($f in $cSrcFiles) {
    $o = [System.IO.Path]::GetFileNameWithoutExtension($f) + ".o"
    Write-Host "  CC    $f"
    & $CC @CFLAGS -c "$Tmp\$f" -o "$Tmp\obj\$o" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $f"
        exit 1
    }
}
Write-Host "OK - all C files compiled"

Write-Host "=== Linking ==="
$objs = $cSrcFiles | ForEach-Object { 
    $base = [System.IO.Path]::GetFileNameWithoutExtension($_)
    "$Tmp\obj\$base.o"
}
& $CC @objs @LDFLAGS -o "$Tmp\build\ldtoypad.prx" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "LINK FAILED"
    exit 1
}

Write-Host "=== PRX built ==="
Get-ChildItem "$Tmp\build\ldtoypad.prx" | Select Length

# Copy PRX to source build/ dir FIRST so sign.sh can find it
New-Item -ItemType Directory -Force -Path "$SrcRoot\build" | Out-Null
Copy-Item "$Tmp\build\ldtoypad.prx" "$SrcRoot\build\ldtoypad.prx" -Force
Write-Host "PRX copied to $SrcRoot\build\ ($((Get-Item "$SrcRoot\build\ldtoypad.prx").Length) bytes)"

Write-Host "=== Signing with oscetool (WSL) ==="
$result = & wsl.exe bash -c "cd '/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin' && bash sign.sh ldtoypad" 2>&1
Write-Host $result

Write-Host "=== Build complete ==="
