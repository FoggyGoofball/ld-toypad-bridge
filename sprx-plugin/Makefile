# Makefile for LD-ToyPad Bridge .sprx plugin
# Uses PSL1GHT SDK toolchain
#
# Architecture: CellOS background thread + Extra LDD registration.
# No LV2 kernel patching required.
#
# CRT: Uses custom crt0.S alongside the stock lv2-sprx.o from PSL1GHT.
# lv2-sprx.o provides the .sys_proc_prx_param metadata block (magic
# 0x1B434CEC) with correct R_PPC64_ADDR32 relocations for the
# __libentstart/__libentend/__libstubstart/__libstubend symbols.
# crt0.S provides _start, the .lib.ent export table (module_start/
# module_stop), and empty .lib.stub markers for sprxlinker to fill.
# crt0.S does NOT use .opd (function descriptors) — PSL1GHT's PPU
# ELF ABI uses direct function pointers, not AIX-style descriptors.

.RECIPEPREFIX := >

PS3DEV   ?= /usr/local/ps3dev
PSL1GHT  ?= $(PS3DEV)/psl1ght

CROSS := powerpc64-ps3-elf-

CC       := $(PS3DEV)/ppu/bin/$(CROSS)gcc
AS       := $(PS3DEV)/ppu/bin/$(CROSS)as
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
# Source files
# ---------------------------------------------------------------
C_SRCS := main.c ldd_driver.c network.c toypad_state.c debug.c compat.c
S_SRCS := crt0.S
C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)
S_OBJS := $(S_SRCS:%.S=$(OBJDIR)/%.o)

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

# Assembler flags: we need the preprocessor to process #include in .S files,
# and -D__SPRX__ to match CFLAGS for consistency.
ASFLAGS := $(MACHDEP) -g -D__SPRX__ $(INCLUDES)

# Linker flags:
# -nostartfiles: we provide our own _start in crt0.S -> crt0.o
# -nostdlib -nodefaultlibs: no standard CRT or libc
# No -fPIE, -pie, or -Wl,-q:
#   VSH SPRX modules use the standard PSL1GHT ET_EXEC model.  The
#   kernel PRX loader and sprxlinker handle all relocation through
#   the NID import/export tables (.lib.ent / .lib.stub) and the
#   .sys_proc_prx_param metadata provided by lv2-sprx.o.
#   PIE-style relocations are not applicable to kernel PRX modules.
#
# lv2-sprx.o provides the correct .sys_proc_prx_param section (magic
# 0x1B434CEC) with R_PPC64_ADDR32 relocation entries for the
# __libentstart/__libentend/__libstubstart/__libstubend symbols.
# crt0.o provides those symbols plus _start, .lib.ent, and .lib.stub.
LDFLAGS := $(MACHDEP) -nostartfiles -nostdlib -nodefaultlibs \
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

$(OBJDIR)/%.o: %.S
>@echo "  AS    $<"
>$(CC) $(ASFLAGS) -c $< -o $@

$(BUILDDIR)/$(TARGET).elf: $(S_OBJS) $(C_OBJS)
>@echo "  LD    $@"
>$(LD) $(S_OBJS) $(C_OBJS) $(LDFLAGS) -o $@

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
