# Makefile for Hello World SPRX Buzz Diagnostic Stub
# Build with Sony PS3 DUPLEX SDK (SN Systems / SDK 3.40)
# Package with oscetool via WSL
#
# oscetool uses SHORT flags matching sign.sh:
#   -0 SELF         (sce-type)
#   -1 TRUE         (compress-data)
#   -2 000A         (key-revision)
#   -3 1010000001000003  (self-auth-id)
#   -4 01000002     (self-vendor-id)
#   -5 APP          (self-type)
#   -6 0004009300000000  (self-fw-version)
#   -A 0001000000000000  (self-app-version)
#   -8 4000000000...     (self-ctrl-flags)

CELL_SDK   ?= /mnt/c/usr/local/cell
CELL_HOST   = $(CELL_SDK)/host-win32
CELL_TARGET = $(CELL_SDK)/target

CROSS_PREFIX := $(CELL_HOST)/ppu/ppu-lv2

CC       := $(CROSS_PREFIX)/bin/gcc.exe
LD       := $(CROSS_PREFIX)/bin/gcc.exe
OSCETOOL := $(HOME)/oscetool-src/oscetool

TARGET   := helloworld
BUILDDIR := build
OBJDIR   := obj

C_SRCS   := main.c
C_OBJS   := $(C_SRCS:%.c=$(OBJDIR)/%.o)

INCLUDES := -I$(CELL_TARGET)/ppu/include

# CRITICAL: -fno-builtin -nodefaultlibs prevents TLS/stack canary crashes
# -nodefaultlibs retains prx_crt0.o (provides _start, .sys_proc_prx_param).
CFLAGS := -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $(INCLUDES)
LDFLAGS := -mprx -nodefaultlibs -llv2_stub

# oscetool short flags matching sign.sh / ldtoypad signing
SCETOOL_FLAGS := \
	-0 SELF -1 TRUE \
	-2 000A -3 1010000001000003 \
	-4 01000002 \
	-A 0001000000000000 -5 APP \
	-8 4000000000000000000000000000000000000000000000000000000000000002 \
	-6 0004009300000000

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
	mkdir -p "$(OBJDIR)" "$(BUILDDIR)"

$(OBJDIR)/%.o: main.c
	@echo "  CC    $<"
	$(CC) $(CFLAGS) -c "$<" -o "$@"

$(BUILDDIR)/$(TARGET).prx: $(C_OBJS)
	@echo "  LD    $@"
	$(LD) $(C_OBJS) $(LDFLAGS) -o "$@"

$(BUILDDIR)/$(TARGET).sprx: $(BUILDDIR)/$(TARGET).prx
	@echo "  SPRX  $@ (via oscetool)"
	cp "$(CURDIR)/$(BUILDDIR)/$(TARGET).prx" /tmp/$(TARGET).prx
	cd $(dir $(OSCETOOL)) && $(OSCETOOL) -v $(SCETOOL_FLAGS) \
		-e /tmp/$(TARGET).prx /tmp/$(TARGET).sprx
	cp /tmp/$(TARGET).sprx "$(CURDIR)/$(BUILDDIR)/$(TARGET).sprx"
	ls -la "$(BUILDDIR)/$(TARGET).sprx"
	@echo "Build complete: $(BUILDDIR)/$(TARGET).sprx"

clean:
	@echo "  CLEAN"
	rm -rf "$(OBJDIR)" "$(BUILDDIR)" *.prx *.sprx

rebuild: clean all

.PHONY: all clean rebuild dirs
