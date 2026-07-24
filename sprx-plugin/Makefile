# Makefile for LD-ToyPad Bridge SPRX
# Build with Sony PS3 DUPLEX SDK (SN Systems / SDK 3.40)
# Package with oscetool via WSL
#
# REFACTORED 2026-07-22:
#   - Removed toc_trampoline.s and toc_trampoline_c.c (assembly wrappers)
#   - Added trampoline_gen.c (dynamic PowerPC trampoline generator)
#   - Assembly-free: all TOC management done at runtime via trampoline_gen.c

CELL_SDK   ?= /mnt/c/usr/local/cell
CELL_HOST   = $(CELL_SDK)/host-win32
CELL_TARGET = $(CELL_SDK)/target

CROSS_PREFIX := $(CELL_HOST)/ppu/ppu-lv2

CC       := $(CROSS_PREFIX)/bin/gcc.exe
LD       := $(CROSS_PREFIX)/bin/gcc.exe
WSL      := wsl.exe
MSYS2_ARG_CONV_EXCL := asterisk

TARGET   := ldtoypad
BUILDDIR := build
OBJDIR   := obj

C_SRCS   := main.c compat.c network.c debug.c toypad_state.c usb_hooks.c trampoline_gen.c
C_OBJS   := $(C_SRCS:%.c=$(OBJDIR)/%.o)
# No .s assembly files needed — all trampolines generated dynamically at runtime

INCLUDES := -I$(CELL_TARGET)/ppu/include

CFLAGS := -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $(INCLUDES)
LDFLAGS := -mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lusbd_stub

# oscetool flags for SPRX signing
# Note: -5 PRX per expert recommendation for injectable modules.
SCETOOL_FLAGS := \
	-0 SELF -1 TRUE \
	-2 000A -3 1010000001000003 \
	-4 01000002 \
	-A 0001000000000000 \
	-5 PRX \
	-8 4000000000000000000000000000000000000000000000000000000000000002 \
	-6 0004009300000000

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
	mkdir -p "$(OBJDIR)" "$(BUILDDIR)"

$(OBJDIR)/%.o: %.c
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(BUILDDIR)/$(TARGET).prx: $(C_OBJS)
	@echo "  LD    $@"
	$(LD) $(C_OBJS) $(LDFLAGS) -o "$@"

$(BUILDDIR)/$(TARGET).sprx: $(BUILDDIR)/$(TARGET).prx
	@echo "  SPRX  $@ (via sign.sh)"
	$(WSL) bash -c "cd \"/mnt/c/Users/Admin/source/repos/dimensions plugin/sprx-plugin\" && bash sign.sh $(TARGET)"
	ls -la "$(BUILDDIR)/$(TARGET).sprx"
	@echo "Build complete: $(BUILDDIR)/$(TARGET).sprx"

clean:
	@echo "  CLEAN"
	rm -rf "$(OBJDIR)" "$(BUILDDIR)" *.prx *.sprx

rebuild: clean all

.PHONY: all clean rebuild dirs
