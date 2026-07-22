# Makefile for LD-ToyPad Bridge SPRX
# Build with Sony PS3 DUPLEX SDK (SN Systems / SDK 3.40)
# Package with oscetool via WSL

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

C_SRCS   := main.c compat.c network.c debug.c toypad_state.c usb_hooks.c toc_trampoline_c.c
C_OBJS   := $(C_SRCS:%.c=$(OBJDIR)/%.o)
S_SRCS   := toc_trampoline.s
S_OBJS   := $(S_SRCS:%.s=$(OBJDIR)/%.o)

INCLUDES := -I$(CELL_TARGET)/ppu/include

CFLAGS := -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $(INCLUDES)
LDFLAGS := -mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lusbd_stub

# oscetool flags for SPRX signing (APP = application/PRX type)
# Note: -5 APP is required for this oscetool version (0.9.2).
# -5 ISLAND causes "Error: Invalid SELF type" on this toolchain.
SCETOOL_FLAGS := \
	-0 SELF -1 TRUE \
	-2 000A -3 1010000001000003 \
	-4 01000002 \
	-A 0001000000000000 \
	-5 APP \
	-8 4000000000000000000000000000000000000000000000000000000000000002 \
	-6 0004009300000000

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
	mkdir -p "$(OBJDIR)" "$(BUILDDIR)"

$(OBJDIR)/%.o: %.c
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(OBJDIR)/%.o: %.s
	@echo "  AS    $<"
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(BUILDDIR)/$(TARGET).prx: $(C_OBJS) $(S_OBJS)
	@echo "  LD    $@"
	$(LD) $(C_OBJS) $(S_OBJS) $(LDFLAGS) -o "$@"

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
