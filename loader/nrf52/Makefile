CROSS ?= arm-none-eabi-
LAYOUT ?= S140_V7
BOARD ?=
ORIGIN_S140_V7 = 0x27000
ORIGIN_S140_V6 = 0x26000
PAGES ?= 1
COMPACT ?=
OUT ?= build
CFLAGS = -mcpu=cortex-m4 -mthumb -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -Wall -Wextra -Werror -std=c99 -DMCOTA_LAYOUT_$(LAYOUT) $(if $(BOARD),-DMCOTA_BOARD_$(BOARD)) $(if $(COMPACT),-DMCOTA_BOOT_COMPACT)
LDFLAGS = -nostdlib -nostartfiles -Wl,--gc-sections -Wl,--defsym=LOADER_ORIGIN=$(ORIGIN_$(LAYOUT)) -Wl,--defsym=LOADER_SIZE=$(PAGES)*4096 -T loader.ld
SRC = main.c ../../core/mcota_boot.c ../../core/mcota_sha256.c $(if $(COMPACT),../../core/mcota_compact.c ../../core/mcota_manifest.c ../../core/mcota_patch.c)

$(OUT)/loader_$(LAYOUT).bin: $(SRC) loader.ld layout.h xflash.h ../../core/mcota_boot.h
	mkdir -p $(OUT)
	$(CROSS)gcc $(CFLAGS) $(LDFLAGS) -o $(OUT)/loader_$(LAYOUT).elf $(SRC) -lgcc
	$(CROSS)objcopy -O binary $(OUT)/loader_$(LAYOUT).elf $@
	$(CROSS)size $(OUT)/loader_$(LAYOUT).elf

clean:
	rm -rf $(OUT)

.PHONY: clean
