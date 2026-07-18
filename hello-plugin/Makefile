# Makefile for Hello World SPRX Buzz Diagnostic Stub
# Build with Sony PS3 DUPLEX SDK (SN Systems / SDK 3.40)
# Package with scetool from PSL1GHT toolchain via WSL
# Requires: PS3 environment variable set in WSL pointing to keys directory
#   e.g. add to ~/.bashrc: export PS3=$HOME/.scetool

CELL_SDK   ?= C:/usr/local/cell
CELL_HOST   = $(CELL_SDK)/host-win32
CELL_TARGET = $(CELL_SDK)/target

CROSS_PREFIX := $(CELL_HOST)/ppu/ppu-lv2

CC       := $(CROSS_PREFIX)/bin/gcc.exe
LD       := $(CROSS_PREFIX)/bin/gcc.exe
SCETOOL  := wsl /usr/local/ps3dev/bin/scetool

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

# scetool flags matching working webftp_server.sprx on Evilnat 4.93:
#   --key-revision=000A    : From working webftp (0x000A)
#   --self-auth-id=1010000001000003 : vsh/module modules (webftp matching)
#   --self-type=APP        : Application type (matching webftp)
#   --self-ctrl-flags=4000000000000000000000000000000000000000000000000000000000000002 : From webftp dump
SCETOOL_FLAGS := -v --sce-type=SELF --compress-data=TRUE \
	--key-revision=000A --self-auth-id=1010000001000003 \
	--self-vendor-id=01000002 \
	--self-app-version=0001000000000000 --self-type=APP \
	--self-ctrl-flags=4000000000000000000000000000000000000000000000000000000000000002 \
	--self-fw-version=0004009300000000

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
	@echo "  SPRX  $@ (via scetool)"
	$(SCETOOL) $(SCETOOL_FLAGS) \
		-e "$$(wslpath -a '$(CURDIR)/$(BUILDDIR)/$(TARGET).prx')" \
		"$$(wslpath -a '$(CURDIR)/$(BUILDDIR)/$(TARGET).sprx')"
	@echo "Build complete: $(BUILDDIR)/$(TARGET).sprx"

clean:
	@echo "  CLEAN"
	rm -rf "$(OBJDIR)" "$(BUILDDIR)"

rebuild: clean all

.PHONY: all clean rebuild dirs
