# Build script for LD-ToyPad Bridge SPRX
# Copies sources to temp dir (no spaces), compiles, links, copies back
# Includes assembly trampoline (toc_trampoline.s)

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

Write-Host "=== Compiling C sources ==="
$cFiles = @("main.c","compat.c","network.c","debug.c","toypad_state.c","usb_hooks.c","toc_trampoline_c.c")
foreach ($f in $cFiles) {
    $o = [System.IO.Path]::GetFileNameWithoutExtension($f) + ".o"
    Write-Host "  CC    $f"
    & $CC @CFLAGS -c "$Tmp\$f" -o "$Tmp\obj\$o" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $f"
        exit 1
    }
}
Write-Host "OK - all C files compiled"

Write-Host "=== Assembling toc_trampoline.s ==="
& $CC @CFLAGS -c "$Tmp\toc_trampoline.s" -o "$Tmp\obj\toc_trampoline.o" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: toc_trampoline.s"
    exit 1
}
Write-Host "OK - assembly assembled"

Write-Host "=== Linking ==="
$objs = @(
    "$Tmp\obj\main.o","$Tmp\obj\compat.o","$Tmp\obj\network.o",
    "$Tmp\obj\debug.o","$Tmp\obj\toypad_state.o",
    "$Tmp\obj\usb_hooks.o",
    "$Tmp\obj\toc_trampoline.o",
    "$Tmp\obj\toc_trampoline_c.o"
)
& $CC @objs @LDFLAGS -o "$Tmp\build\ldtoypad.prx" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "LINK FAILED"
    exit 1
}

Write-Host "=== PRX built ==="
Get-ChildItem "$Tmp\build\ldtoypad.prx" | Select Length

Write-Host "=== Copying to sprx-plugin/build/ ==="
New-Item -ItemType Directory -Force -Path "$SrcRoot\build" | Out-Null
Copy-Item "$Tmp\build\ldtoypad.prx" "$SrcRoot\build\ldtoypad.prx" -Force
Write-Host "OK - $((Get-Item "$SrcRoot\build\ldtoypad.prx").Length) bytes"

Write-Host "=== Build complete ==="
