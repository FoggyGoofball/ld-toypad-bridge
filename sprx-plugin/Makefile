# Makefile for LD-ToyPad SPRX Plugin (Sony SDK / make_self)
CELL_SDK   ?= C:/usr/local/cell
CELL_HOST   = $(CELL_SDK)/host-win32
CELL_TARGET = $(CELL_SDK)/target
CROSS_PREFIX := $(CELL_HOST)/ppu/ppu-lv2
CC       := $(CROSS_PREFIX)/bin/gcc.exe
LD       := $(CROSS_PREFIX)/bin/gcc.exe
MAKESELF := wsl /usr/local/ps3dev/bin/make_self
TARGET   := ldtoypad
BUILDDIR := build
OBJDIR   := obj
C_SRCS   := main.c ldd_driver.c network.c toypad_state.c debug.c compat.c
C_OBJS   := $(C_SRCS:%.c=$(OBJDIR)/%.o)
INCLUDES := -I$(CELL_TARGET)/ppu/include
CFLAGS := -mprx -std=gnu99 -O2 -g -fno-builtin -nodefaultlibs $(INCLUDES)
LDFLAGS := -mprx -nodefaultlibs -llv2_stub -lfs_stub -lnet_stub -lsysmodule_stub -lusbd_stub -lc_stub
all: dirs $(BUILDDIR)/$(TARGET).sprx
dirs:
	mkdir -p $(OBJDIR) $(BUILDDIR)
$(OBJDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILDDIR)/$(TARGET).prx: $(C_OBJS)
	$(LD) $(C_OBJS) $(LDFLAGS) -o $@
$(BUILDDIR)/$(TARGET).sprx: $(BUILDDIR)/$(TARGET).prx
	$(MAKESELF) $(shell wslpath -a $(CURDIR)/$(BUILDDIR)/$(TARGET).prx) $(shell wslpath -a $(CURDIR)/$(BUILDDIR)/$(TARGET).sprx)
clean:
	rm -rf $(OBJDIR) $(BUILDDIR)
rebuild: clean all
.PHONY: all clean rebuild dirs
