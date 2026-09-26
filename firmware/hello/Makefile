# fb200-hello firmware build. Booting, linking and vectors are ours
# (linker.ld, src/startup.c, src/vectors.c); TinyUSB, its imxrt BSP and the
# MCUXpresso SDK driver subset are compiled directly from the pinned
# .deps/tinyusb tree, never from vendored copies.
TARGET       := build/fb200-hello
TUSB_DIR     := .deps/tinyusb
MCUX_CORE    := $(TUSB_DIR)/hw/mcu/nxp/mcuxsdk-core
MCUX_DEVICES := $(TUSB_DIR)/hw/mcu/nxp/mcux-devices-rt
CMSIS_DIR    := $(TUSB_DIR)/lib/CMSIS_6
BOARD_DIR    := $(TUSB_DIR)/hw/bsp/imxrt/boards/mimxrt1060_evk
MCU_DIR      := $(MCUX_DEVICES)/RT1060/MIMXRT1062

# MCUXpresso SDK subset the imxrt BSP needs. The commit pins live in the
# checksum-verified TinyUSB tarball (tools/get_deps.py).
SDK_DEPS  := hw/mcu/nxp/mcuxsdk-core hw/mcu/nxp/mcux-devices-rt lib/CMSIS_6
SDK_STAMP := $(TUSB_DIR)/.mcux-sdk-fetched

-include tinyusb.lock
TINYUSB_URL ?= https://github.com/hathach/tinyusb/archive/refs/tags/$(TINYUSB_TAG).tar.gz

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm

CPUFLAGS  := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
TUSB_INC  := \
  -I $(TUSB_DIR)/src \
  -I $(TUSB_DIR)/hw -I $(TUSB_DIR)/hw/bsp \
  -I $(BOARD_DIR) -I $(BOARD_DIR)/board \
  -I $(CMSIS_DIR)/CMSIS/Core/Include \
  -I $(MCU_DIR) -I $(MCU_DIR)/drivers \
  -I $(MCUX_DEVICES)/RT1060/periph \
  -I $(MCUX_CORE)/drivers/common \
  -I $(MCUX_CORE)/drivers/igpio \
  -I $(MCUX_CORE)/drivers/lpuart \
  -I $(MCUX_CORE)/drivers/ocotp
TUSB_DEFS := \
  -DCFG_TUSB_MCU=OPT_MCU_MIMXRT10XX \
  -DCPU_MIMXRT1062DVL6A \
  -DXIP_EXTERNAL_FLASH=1 \
  -DNDEBUG
CFLAGS    := $(CPUFLAGS) -O2 -g3 -ffreestanding -fno-builtin -fno-common \
             -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
             -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
             $(TUSB_INC) $(TUSB_DEFS) -I src -I src/compat -I board -I . -MMD -MP
LDFLAGS   := $(CPUFLAGS) -nostdlib -T linker.ld -Wl,-Map=$(TARGET).map,--cref

SRC_C := \
  src/startup.c src/vectors.c src/main.c src/memfuncs.c \
  src/system_clock.c src/usb_descriptors.c src/compat/stubs.c \
  $(TUSB_DIR)/src/tusb.c \
  $(TUSB_DIR)/src/common/tusb_fifo.c \
  $(TUSB_DIR)/src/device/usbd.c \
  $(TUSB_DIR)/src/class/cdc/cdc_device.c \
  $(TUSB_DIR)/src/portable/chipidea/ci_hs/dcd_ci_hs.c \
  $(TUSB_DIR)/hw/bsp/board.c \
  $(TUSB_DIR)/hw/bsp/imxrt/family.c \
  $(BOARD_DIR)/board/clock_config.c \
  $(BOARD_DIR)/board/pin_mux.c \
  $(MCUX_CORE)/drivers/common/fsl_common.c \
  $(MCUX_CORE)/drivers/common/fsl_common_arm.c \
  $(MCUX_CORE)/drivers/igpio/fsl_gpio.c \
  $(MCUX_CORE)/drivers/lpuart/fsl_lpuart.c \
  $(MCUX_CORE)/drivers/ocotp/fsl_ocotp.c \
  $(MCU_DIR)/drivers/fsl_clock.c
SRC_S := src/stage2.S
OBJS  := $(patsubst %,$(TARGET)/%.o,$(SRC_C) $(SRC_S))

.PHONY: all build clean layout deps

# Order-only: every object must wait for dependency fetching on a fresh tree,
# but a re-fetch must not force recompilation.
$(OBJS): | deps

all: build

build: deps $(TARGET).vectors.bin $(TARGET).blob.bin

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -lgcc -o $@

# .vectors goes to block 0 offset 0; .blob to offset 0x7d4 (LMA in linker.ld).
$(TARGET).vectors.bin $(TARGET).blob.bin: $(TARGET).elf
	$(OBJCOPY) -O binary -j .vectors $< $(TARGET).vectors.bin
	$(OBJCOPY) -O binary -j .blob $< $(TARGET).blob.bin

$(TARGET)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

layout: deps $(TARGET).elf
	$(NM) $(TARGET).elf | grep -E '(_estack|__bss_|__blob_end__|stage2|app_main)' | sort > build/layout.txt
	cat build/layout.txt

clean:
	rm -rf build

deps: $(TUSB_DIR)/src/tusb.c $(SDK_STAMP)

$(TUSB_DIR)/src/tusb.c: tinyusb.lock
	@mkdir -p .deps
	curl -fsSL -o .deps/tinyusb.tar.gz "$(TINYUSB_URL)"
	echo "$(TINYUSB_SHA256)  .deps/tinyusb.tar.gz" | shasum -a 256 -c - || \
	  echo "$(TINYUSB_SHA256)  .deps/tinyusb.tar.gz" | sha256sum -c -
	@rm -rf .deps/tinyusb .deps/tinyusb.tmp && mkdir -p .deps/tinyusb.tmp
	tar -xzf .deps/tinyusb.tar.gz -C .deps/tinyusb.tmp --strip-components=1
	mv .deps/tinyusb.tmp $(TUSB_DIR)
	@touch $@   # tarball mtimes predate the lock; marker = fetch time

# MCUXpresso SDK subset for the imxrt BSP, fetched at the commits pinned inside
# the checksum-verified TinyUSB tarball. Only these three repos are needed.
$(SDK_STAMP): $(TUSB_DIR)/src/tusb.c
	python3 -c "import sys; sys.path.insert(0, '$(TUSB_DIR)/tools'); import get_deps; \
	list(map(get_deps.get_a_dep, '$(SDK_DEPS)'.split()))"
	@for f in $(MCUX_CORE)/drivers/common/fsl_common.c \
	  $(MCUX_DEVICES)/RT1060/MIMXRT1062/MIMXRT1062.h \
	  $(CMSIS_DIR)/CMSIS/Core/Include/core_cm7.h; do \
	  test -f "$$f" || { echo "ERROR: MCUX SDK fetch failed: $$f"; exit 1; }; \
	done
	@touch $@

-include $(OBJS:.o=.d)
