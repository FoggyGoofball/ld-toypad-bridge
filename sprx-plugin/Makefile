# Makefile for LD-ToyPad SPRX Plugin (Sony SDK + oscetool signing)
# Run from WSL: cd /mnt/c/Users/Admin/source/repos/dimensions\ plugin/sprx-plugin && make

CELL_SDK   ?= /mnt/c/usr/local/cell
CELL_HOST   = $(CELL_SDK)/host-win32
CELL_TARGET = $(CELL_SDK)/target
CROSS_PREFIX := $(CELL_HOST)/ppu/ppu-lv2
CC       := $(CROSS_PREFIX)/bin/gcc.exe
LD       := $(CROSS_PREFIX)/bin/gcc.exe
SCETOOL  := $(HOME)/oscetool-src/oscetool
TARGET   := ldtoypad
BUILDDIR := build
OBJDIR   := obj
C_SRCS   := main.c ldd_driver.c network.c toypad_state.c debug.c compat.c
C_OBJS   := $(C_SRCS:%.c=$(OBJDIR)/%.o)
INCLUDES := -I$(CELL_TARGET)/ppu/include
CFLAGS := -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $(INCLUDES)
LDFLAGS := -mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lsysmodule_stub -lusbd_stub -lc_stub

# scetool flags matching working hello-plugin build (Evilnat 4.93 Cobra 8.5)
# NOTE: oscetool uses SHORT flags (-0, -1, -2, etc.), NOT long flags (--sce-type, etc.)
SCETOOL_FLAGS := -0 SELF -1 TRUE \
	-2 000A -3 1010000001000003 \
	-4 01000002 \
	-A 0001000000000000 -5 APP \
	-8 4000000000000000000000000000000000000000000000000000000000000002 \
	-6 0004009300000000

SCETOOL_DIR := $(HOME)/oscetool-src
OSCETOOL    := $(SCETOOL_DIR)/oscetool

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
	@echo "  SPRX  $@"
	cp "$(CURDIR)/$(BUILDDIR)/$(TARGET).prx" /tmp/$(TARGET).prx
	cd $(SCETOOL_DIR) && $(OSCETOOL) -v $(SCETOOL_FLAGS) \
		-e /tmp/$(TARGET).prx /tmp/$(TARGET).sprx
	cp /tmp/$(TARGET).sprx "$(CURDIR)/$(BUILDDIR)/$(TARGET).sprx"
	ls -la "$(BUILDDIR)/$(TARGET).sprx"
	@echo "Build complete: $(BUILDDIR)/$(TARGET).sprx"


clean:
	@echo "  CLEAN"
	rm -rf "$(OBJDIR)" "$(BUILDDIR)" *.prx *.sprx

rebuild: clean all

.PHONY: all clean rebuild dirs
