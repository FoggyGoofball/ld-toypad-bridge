# Makefile for LD-ToyPad Bridge .sprx plugin
# Uses PSL1GHT SDK toolchain
#
# Architecture: CellOS background thread + Extra LDD registration.
# No LV2 kernel patching required.
#
# CRT: Uses the stock PSL1GHT lv2-sprx.o startup object for the
# _start entry point, .sys_proc_prx_param metadata (magic 0x1B434CEC)
# with correct R_PPC64_ADDR32 relocations for the
# __libentstart/__libentend/__libstubstart/__libstubend symbols.
# PRX macros in main.c (SYS_MODULE_INFO/START/STOP from <sys/prx.h>)
# generate the .rodata.sceModuleInfo section and .lib.ent export table.
# No custom crt0.S is needed.

.RECIPEPREFIX := >

PS3DEV   ?= /usr/local/ps3dev
PSL1GHT  ?= $(PS3DEV)/psl1ght

CROSS := powerpc64-ps3-elf-

CC       := $(PS3DEV)/ppu/bin/$(CROSS)gcc
LD       := $(PS3DEV)/ppu/bin/$(CROSS)gcc
STRIP    := $(PS3DEV)/ppu/bin/$(CROSS)strip
OBJCOPY  := $(PS3DEV)/ppu/bin/$(CROSS)objcopy

SPRX     := $(PS3DEV)/bin/sprxlinker
SELF     := $(PS3DEV)/bin/make_self
FSELF    := $(PS3DEV)/bin/fself

TARGET   := ldtoypad
BUILDDIR := $(CURDIR)/build
OBJDIR   := $(CURDIR)/obj

# ---------------------------------------------------------------
# Source files -- no assembler sources; PRX macros in main.c handle
# the export table generation.
# ---------------------------------------------------------------
C_SRCS := main.c ldd_driver.c network.c toypad_state.c debug.c compat.c
C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)

# Reduced section-splitting avoids ELF PHDR overflow
MACHDEP  := -mhard-float -fmodulo-sched

INCLUDES := -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath

# NO -fPIC, -fPIE, or -shared:
#   PSL1GHT SDK builds SPRX as ET_EXEC (statically-linked).  The kernel
#   PRX loader and sprxlinker both expect ET_EXEC output.  -fPIE and -pie
#   introduce GOT-based addressing designed for userspace process images,
#   which is inappropriate for a VSH plugin background thread that runs
#   in kernel PRX address space.
#
#   The standard PSL1GHT model uses -fno-pic, with relocation metadata
#   provided by lv2-sprx.o (the stock PRX startup object).  The Cobra
#   VSH loader handles memory mapping through its own internal PRX
#   loader, not via ELF PIE relocations.
CFLAGS := $(MACHDEP) -O2 -g -std=gnu99 -fno-pic -fno-stack-protector -fvisibility=hidden -D__SPRX__ \
          $(INCLUDES)

# Linker flags:
# -nostdlib -nodefaultlibs: no standard CRT or libc
# NOTE: -nostartfiles is NOT used. The stock lv2-sprx.o provides the
#   _start entry point. Using -nostartfiles would suppress its
#   inclusion via the default linker script.
# No -fPIE, -pie, or -Wl,-q:
#   VSH SPRX modules use the standard PSL1GHT ET_EXEC model.  The
#   kernel PRX loader and sprxlinker handle all relocation through
#   the NID import/export tables (.lib.ent / .lib.stub) and the
#   .sys_proc_prx_param metadata provided by lv2-sprx.o.
#
# lv2-sprx.o provides:
#   - _start entry point (calls module_start)
#   - .sys_proc_prx_param section (magic 0x1B434CEC) with
#     R_PPC64_ADDR32 relocation entries for the
#     __libentstart/__libentend/__libstubstart/__libstubend symbols
#   - Correct PRX type (0x04) for Cobra VSH compatibility
#
# The PRX macros in main.c (SYS_MODULE_INFO/START/STOP) generate
# the .rodata.sceModuleInfo section and .lib.ent export table
# with proper NID entries for module_start/module_stop.
LDFLAGS := $(MACHDEP) -nostdlib -nodefaultlibs \
           -L$(PSL1GHT)/ppu/lib -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib \
           /usr/local/ps3dev/ppu/powerpc64-ps3-elf/lib/lv2-sprx.o \
           -lusb -lnet -lsysutil -lsysmodule -lsysfs -lrt -llv2 \
           -Wl,--start-group -lgcc -Wl,--end-group

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
>@mkdir -p $(OBJDIR) $(BUILDDIR)

$(OBJDIR)/%.o: %.c
>@echo "  CC    $<"
>$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/$(TARGET).elf: $(C_OBJS)
>@echo "  LD    $@"
>$(LD) $(C_OBJS) $(LDFLAGS) -o $@

# Strip the ELF before feeding to sprxlinker:
#   Bare strip is correct for the standard PSL1GHT SPRX model
#   (ET_EXEC with .lib.ent/.lib.stub NID tables).  There are no
#   ELF .rela.* relocations to preserve — the Cobra kernel PRX
#   loader does not use ELF relocations.  It maps the module
#   through the .sys_proc_prx_param metadata and resolves NID
#   imports via the .lib.stub table that sprxlinker generates.
$(BUILDDIR)/$(TARGET).sprx: $(BUILDDIR)/$(TARGET).elf
>@echo "  SPRX  $@"
>$(STRIP) $< -o $(BUILDDIR)/$(TARGET)-strip.elf
>$(SPRX) $(BUILDDIR)/$(TARGET)-strip.elf
>$(SELF) $(BUILDDIR)/$(TARGET)-strip.elf $@
>$(FSELF) $(BUILDDIR)/$(TARGET)-strip.elf $(BUILDDIR)/$(TARGET).fake.self
>@echo "Build complete: $@"
>ls -la $@ $(BUILDDIR)/$(TARGET).fake.self

clean:
>@echo "  CLEAN"
>rm -rf $(OBJDIR) $(BUILDDIR)

rebuild: clean all

.PHONY: all clean rebuild dirs
