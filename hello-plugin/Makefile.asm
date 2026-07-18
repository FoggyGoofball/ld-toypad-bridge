# Minimal Makefile for pure-assembly SPRX diagnostic
# No PSL1GHT, no lv2-sprx.o, no stub libraries.
# Everything is raw syscalls via PowerPC assembly.

PS3DEV   ?= /usr/local/ps3dev
CC       := $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-gcc
LD       := $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-gcc
STRIP    := $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-strip
OBJDUMP  := $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-objdump
NM       := $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-nm
READELF  := $(PS3DEV)/ppu/bin/powerpc64-ps3-elf-readelf

SPRX     := $(PS3DEV)/bin/sprxlinker
FSELF    := $(PS3DEV)/bin/fself

TARGET   := helloworld
BUILDDIR := $(CURDIR)/build2
OBJDIR   := $(CURDIR)/obj2

# Single assembly source file
ASM_SRCS := startup_fixed.s
ASM_OBJS := $(ASM_SRCS:%.s=$(OBJDIR)/%.o)

# -fPIC: Position Independent Code (required for SPRX)
# -Wl,-q: preserve .rela sections for sprxlinker
# -nostdlib: no standard CRT
# -Wl,-e,_start: entry point

CFLAGS   := -fPIC -g -D__SPRX__
LDFLAGS  := -nostdlib -nodefaultlibs -Wl,-q -Wl,-e,_start

all: dirs $(BUILDDIR)/$(TARGET).sprx

dirs:
	@mkdir -p $(OBJDIR) $(BUILDDIR)

$(OBJDIR)/%.o: %.s
	@echo "  AS    $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/$(TARGET).elf: $(ASM_OBJS)
	@echo "  LD    $@"
	$(LD) $(ASM_OBJS) $(LDFLAGS) -o $@
	@echo "--- ELF diagnostics ---"
	$(READELF) -S $@ | grep -E 'lib\.|rodata\.sce|text'
	$(NM) $@ | grep -E 'libent|libstub|module|sce'
	$(OBJDUMP) -s -j .rodata.sceModuleInfo $@ || echo "WARNING: .rodata.sceModuleInfo NOT FOUND"
	$(OBJDUMP) -s -j .lib.ent $@
	$(OBJDUMP) -s -j .lib.stub $@

$(BUILDDIR)/$(TARGET).sprx: $(BUILDDIR)/$(TARGET).elf
	@echo "  SPRX  $@"
	# Step 1: Copy unstripped ELF for sprxlinker
	cp $< $(BUILDDIR)/$(TARGET)-prx.elf
	# Step 2: sprxlinker processes .rela sections
	$(SPRX) $(BUILDDIR)/$(TARGET)-prx.elf
	# Step 3: Strip
	$(STRIP) $(BUILDDIR)/$(TARGET)-prx.elf -o $(BUILDDIR)/$(TARGET)-strip.elf
	# Step 4: Sign with fself
	$(FSELF) $(BUILDDIR)/$(TARGET)-strip.elf $@
	@echo "Build complete: $@"
	ls -la $@

clean:
	@echo "  CLEAN"
	rm -rf $(OBJDIR) $(BUILDDIR)

rebuild: clean all

.PHONY: all clean rebuild dirs
