# Makefile for LD-ToyPad Bridge SPRX plugin
# Build with Sony PS3 DUPLEX SDK (SN Systems / SDK 3.40)
# Package with make_self from PSL1GHT toolchain via WSL
# Requires: PS3 environment variable set in WSL pointing to keys directory
#   e.g. add to ~/.bashrc: export PS3=$HOME/.scetool
#
# Stub libraries:
#   -llv2_stub       : sys_ppu_thread_*, basic LV2 syscalls
#   -lfs_stub        : cellFsOpen/Write/Close (filesystem)
#   -lnet_stub       : socket/bind/sendto/recvfrom/close (BSD sockets)
#   -lsysmodule_stub : cellSysmoduleLoadModule
#   -lusbd_stub      : cellUsbdInit, cellUsbdRegisterExtraLdd (USB)
#
# compat.c is intentionally excluded: -lc_stub provides memcpy/memset/strlen/close

CELL_SDK   ?= C:/usr/local/cell
CELL_HOST   = $(CELL_SDK)/host-win32
CELL_TARGET = $(CELL_SDK)/target

CROSS_PREFIX := $(CELL_HOST)/ppu/ppu-lv2

CC       := $(CROSS_PREFIX)/bin/gcc.exe
LD       := $(CROSS_PREFIX)/bin/gcc.exe
SCETOOL  := wsl /usr/local/ps3dev/bin/make_self

TARGET   := ldtoypad
BUILDDIR := build
OBJDIR   := obj

C_SRCS   := main.c ldd_driver.c network.c toypad_state.c debug.c
C_OBJS   := $(C_SRCS:%.c=$(OBJDIR)/%.o)

INCLUDES := -I$(CELL_TARGET)/ppu/include

# CRITICAL: -fno-builtin -nodefaultlibs prevents TLS/stack canary crashes
# -nodefaultlibs retains prx_crt0.o (provides _start, .sys_proc_prx_param).
CFLAGS := -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $(INCLUDES)

# Stub libraries needed for LD-ToyPad plugin functions.
# -lusbd_stub added for cellUsbdInit / cellUsbdRegisterExtraLdd.
LDFLAGS := -mprx -nodefaultlibs \
           -llv2_stub -lfs_stub -lnet_stub -lsysmodule_stub -lusbd_stub -lc_stub

# make_self is simple: usage is `make_self input.prx output.sprx`
# No special flags needed.
SCETOOL_FLAGS :=

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
	mkdir -p "$(OBJDIR)" "$(BUILDDIR)"

$(OBJDIR)/%.o: %.c
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(BUILDDIR)/$(TARGET).prx: $(C_OBJS)
	@echo "  LD    $@"
	$(LD) $(C_OBJS) $(LDFLAGS) -o "$@"

# make_self must be run from within WSL with WSL-accessible paths.
# We copy the .prx to a WSL temp (/tmp), sign it, copy the .sprx back.
$(BUILDDIR)/$(TARGET).sprx: $(BUILDDIR)/$(TARGET).prx
	@echo "  SPRX  $@ (via make_self in WSL)"
	wsl bash -c 'cp "$(subst /mnt/c,C:,/)"/"$(subst \,/, $(subst C:,,$(abspath $<)))" /tmp/$(TARGET).prx 2>/dev/null; \
		cp "$(realpath $<)" /tmp/$(TARGET).prx; \
		/usr/local/ps3dev/bin/make_self /tmp/$(TARGET).prx /tmp/$(TARGET).sprx && \
		cp /tmp/$(TARGET).sprx "$(realpath $@)" && \
		echo "  SPRX  Signed: $(realpath $@)"'
	@echo "Build complete: $(BUILDDIR)/$(TARGET).sprx"

clean:
	@echo "  CLEAN"
	rm -rf "$(OBJDIR)" "$(BUILDDIR)"

rebuild: clean all

.PHONY: all clean rebuild dirs
