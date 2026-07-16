# Makefile for LD-ToyPad Bridge .sprx plugin
# Uses PSL1GHT SDK toolchain
#
# Architecture: CellOS background thread + Extra LDD registration.
# No LV2 kernel patching required.

.RECIPEPREFIX := >

PS3DEV   ?= /usr/local/ps3dev
PSL1GHT  ?= $(PS3DEV)/psl1ght

CROSS := powerpc64-ps3-elf-

CC       := $(PS3DEV)/ppu/bin/$(CROSS)gcc
LD       := $(PS3DEV)/ppu/bin/$(CROSS)gcc
STRIP    := $(PS3DEV)/ppu/bin/$(CROSS)strip

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
C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)

# Reduced section-splitting avoids ELF PHDR overflow
MACHDEP  := -mhard-float -fmodulo-sched

INCLUDES := -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath

# NO -fPIC -shared: PSL1GHT SDK builds SPRX as ET_EXEC (not ET_DYN).
# The sprxlinker + make_self pipeline expects this format.
CFLAGS := $(MACHDEP) -O2 -g -fno-pic -fno-stack-protector -fvisibility=hidden -D__SPRX__ \
          $(INCLUDES)

LDFLAGS := $(MACHDEP) -nostartfiles -nostdlib -nodefaultlibs \
           -L$(PSL1GHT)/ppu/lib -L$(PS3DEV)/ppu/lib -L$(PS3DEV)/portlibs/ppu/lib \
           -lusb -lnet -lsysutil -lsysmodule -lsysfs -lrt -llv2 \
           /usr/local/ps3dev/ppu/powerpc64-ps3-elf/lib/lv2-sprx.o -Wl,--start-group -lgcc -Wl,--end-group

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
>@mkdir -p $(OBJDIR) $(BUILDDIR)

$(OBJDIR)/%.o: %.c
>@echo "  CC    $<"
>$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/$(TARGET).elf: $(C_OBJS)
>@echo "  LD    $@"
>$(LD) $(C_OBJS) $(LDFLAGS) -o $@

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
