# Makefile for LD-ToyPad Bridge .sprx plugin
# Uses PSL1GHT SDK toolchain
# Evilnat 4.93 CEX target (USB_HOOK_ENABLE_PATCH=1 for live patching)

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
# Live patch configuration — Evilnat 4.93 CEX
# ---------------------------------------------------------------
USB_HOOK_ENABLE_PATCH      ?= 1      # 1 = LV2 syscall-table patching active
USB_HOOK_STRICT_PATCH      ?= 1      # Fail if any patch step fails
USB_HOOK_SYSCALL_TABLE_BASE ?= 0x800000000000F300  # Evilnat 4.93 CEX syscall table
USB_HOOK_LV2_READ_SC       ?= 8      # lv1_read_kernel
USB_HOOK_LV2_WRITE_SC      ?= 9      # lv2_write_kernel

# USB syscall IDs (stable across CEX firmwares)
#   sys_usbd_open:               845
#   sys_usbd_close:              846
#   sys_usbd_get_descriptor:     847
#   sys_usbd_control_transfer:   848
#   sys_usbd_interrupt_transfer: 849
# ---------------------------------------------------------------

C_SRCS := main.c usb_hooks.c network.c toypad_state.c debug.c compat.c
C_OBJS := $(C_SRCS:%.c=$(OBJDIR)/%.o)

# Reduced section-splitting avoids ELF PHDR overflow in this environment.
MACHDEP  := -mhard-float -fmodulo-sched

INCLUDES := -I$(PSL1GHT)/ppu/include -I$(PSL1GHT)/ppu/include/simdmath

CFLAGS := $(MACHDEP) -O2 -g -fno-pic -fno-stack-protector -fvisibility=hidden -D__SPRX__ \
          -DUSB_HOOK_ENABLE_PATCH=$(USB_HOOK_ENABLE_PATCH) \
          -DUSB_HOOK_STRICT_PATCH=$(USB_HOOK_STRICT_PATCH) \
          -DUSB_HOOK_LV2_READ_SC=$(USB_HOOK_LV2_READ_SC) \
          -DUSB_HOOK_LV2_WRITE_SC=$(USB_HOOK_LV2_WRITE_SC) \
          -DUSB_HOOK_SYSCALL_TABLE_BASE=$(USB_HOOK_SYSCALL_TABLE_BASE) \
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
