# fb200-hello firmware build. Booting, linking and vectors are ours
# (linker.ld, src/startup.c, src/vectors.c); TinyUSB, its imxrt BSP and the
# MCUXpresso SDK driver subset are compiled directly from the pinned
# .deps/tinyusb tree, never from vendored copies.
# VARIANT=recovery builds the resident recovery image (block 0); the default
# builds the app for the slot. See docs/BOOTLOADER.md §4.
VARIANT      ?= app
# BUILD overrides where output goes (default: build/); tests/fwbuild.py passes
# a per-process tmp dir so parallel pytest workers never share one build tree.
BUILD        ?= build
TARGET       := $(BUILD)/fb200-$(VARIANT)
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

# CMSIS-DSP (FFT, biquads, vector math), pinned + checksum-verified like
# TinyUSB. Only the group aggregate sources are compiled; --gc-sections keeps
# just what the firmware references.
DSP_DIR   := .deps/cmsis-dsp
DSP_SRC   := $(DSP_DIR)/Source
DSP_GROUPS := BasicMathFunctions ComplexMathFunctions FastMathFunctions \
              FilteringFunctions TransformFunctions StatisticsFunctions \
              SupportFunctions CommonTables

-include tinyusb.lock
-include cmsis-dsp.lock
TINYUSB_URL ?= https://github.com/hathach/tinyusb/archive/refs/tags/$(TINYUSB_TAG).tar.gz

# Needs a toolchain with newlib (CMSIS-DSP includes <math.h>): Debian/Ubuntu
# gcc-arm-none-eabi + libnewlib-arm-none-eabi, or the Arm GNU Toolchain
# (CROSS=<path>/bin/arm-none-eabi-). Homebrew's arm-none-eabi-gcc has no libc.
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
  -I $(MCUX_CORE)/drivers/ocotp \
  -I $(MCUX_CORE)/drivers/lpi2c \
  -I $(MCUX_CORE)/drivers/sai \
  -I $(MCUX_CORE)/drivers/edma \
  -I $(MCUX_CORE)/drivers/dmamux \
  -I $(MCU_DIR)/drivers/romapi \
  -I $(MCUX_CORE)/drivers/flexspi \
  -I $(MCUX_CORE)/drivers/adc_12b1msps_sar \
  -I $(MCUX_CORE)/drivers/flexio -I $(MCUX_CORE)/drivers/flexio/uart \
  -I $(DSP_DIR)/Include -I $(DSP_DIR)/PrivateInclude
TUSB_DEFS := \
  -DCFG_TUSB_MCU=OPT_MCU_MIMXRT10XX \
  -DCPU_MIMXRT1062DVL6A \
  -DXIP_EXTERNAL_FLASH=1 \
  -DNDEBUG \
  -DI2C_RETRY_TIMES=100000 \
  -DARM_MATH_LOOPUNROLL
CFLAGS    := $(CPUFLAGS) -O2 -g3 -ffreestanding -fno-builtin -fno-common \
             -ffunction-sections -fdata-sections -fstack-usage \
             -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
             -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
             $(if $(filter recovery,$(VARIANT)),-DFB200_RECOVERY) \
             $(TUSB_INC) $(TUSB_DEFS) -I src -I src/compat -I board -I . -MMD -MP
LDFLAGS   := $(CPUFLAGS) -nostdlib -Wl,--gc-sections -L $(TARGET) -T linker.ld \
             -Wl,-Map=$(TARGET).map,--cref

SRC_C := \
  src/startup.c src/vectors.c src/main.c src/memfuncs.c \
  src/system_clock.c src/usb_descriptors.c src/compat/stubs.c src/led.c src/crc32.c src/cpu_idle.c src/cpu_power.c \
  src/dsp/dsp.c src/dsp/math.c src/dsp/gain.c src/dsp/testgen.c \
  src/dsp/conv.c src/dsp/conv2.c src/dsp/biquad.c src/dsp/detector.c src/dsp/gate.c src/dsp/comp.c \
  src/dsp/mod.c src/dsp/reverb.c src/dsp/delay.c src/dsp/eq.c \
  src/dsp/drums.c src/dsp/tuner.c src/dsp/looper.c src/dsp/loop_mem.c \
  src/dsp/amp.c src/dsp/tone.c src/dsp/cab.c src/dsp/stock_data.c \
  src/debug/log_fmt.c src/debug/cdc_log.c src/debug/console.c \
  src/debug/selfupdate.c src/debug/flash_rmw.c src/debug/recovery.c src/debug/regs.c \
  src/audio/i2c_probe.c \
  src/audio/usb_audio.c src/audio/engine.c src/audio/frontend.c src/audio/bt_audio.c \
  src/ui/pads.c src/ui/display.c src/ui/controls.c src/ui/power.c src/ui/power_logic.c \
  src/ui/ui.c src/ui/lightbar.c src/ui/rgb.c src/preset/preset.c src/preset/preset_check.c src/bt/bt.c src/audio/codec.c \
  src/proto/proto.c src/proto/proto_port.c src/usb_hid.c \
  src/irstore/irstore.c src/irstore/irstore_port.c \
  src/audio/sai.c src/audio/drift.c \
  $(TUSB_DIR)/src/tusb.c \
  $(TUSB_DIR)/src/common/tusb_fifo.c \
  $(TUSB_DIR)/src/device/usbd.c \
  $(TUSB_DIR)/src/class/cdc/cdc_device.c \
  $(TUSB_DIR)/src/class/hid/hid_device.c \
  $(TUSB_DIR)/src/class/audio/audio_device.c \
  $(MCUX_CORE)/drivers/sai/fsl_sai.c \
  $(MCUX_CORE)/drivers/sai/fsl_sai_edma.c \
  $(MCUX_CORE)/drivers/dmamux/fsl_dmamux.c \
  $(MCUX_CORE)/drivers/edma/fsl_edma.c \
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
  $(MCUX_CORE)/drivers/lpi2c/fsl_lpi2c.c \
  $(MCU_DIR)/drivers/fsl_clock.c \
  $(MCUX_CORE)/drivers/flexspi/fsl_flexspi.c \
  $(MCUX_CORE)/drivers/adc_12b1msps_sar/fsl_adc.c \
  $(MCUX_CORE)/drivers/flexio/fsl_flexio.c \
  $(MCUX_CORE)/drivers/flexio/uart/fsl_flexio_uart.c \
  $(MCUX_CORE)/drivers/flexio/uart/fsl_flexio_uart_edma.c
# The recovery image is a resident launcher: it must stay below ITCM 0x1F000
# (the copier), so it carries none of the audio/DSP/LED code.
ifeq ($(VARIANT),recovery)
SRC_C := $(filter-out src/led.c src/cpu_idle.c src/cpu_power.c src/dsp/% src/audio/engine.c \
                    src/audio/usb_audio.c src/audio/codec.c src/audio/sai.c \
                    src/audio/drift.c src/audio/frontend.c src/audio/bt_audio.c src/ui/% src/preset/% src/bt/% src/proto/% \
                    src/irstore/% \
                    $(MCUX_CORE)/drivers/adc_12b1msps_sar/% $(MCUX_CORE)/drivers/flexio/% \
                    $(MCUX_CORE)/drivers/sai/% $(MCUX_CORE)/drivers/edma/% \
                    $(MCUX_CORE)/drivers/dmamux/%,$(SRC_C))
endif

ifneq ($(VARIANT),recovery)
SRC_C += $(foreach g,$(DSP_GROUPS),$(DSP_SRC)/$(g)/$(g).c)
endif

# Stock data (amp models, cab IRs, tone stack, drum rhythms) is vendor data:
# no image contains it. It lives in its own flash area, written from the
# user's stock .mr (src/dsp/stock_data.h, `fb200 update stock`).
STOCK_OBJS  := $(patsubst %,$(TARGET)/%.o,src/dsp/amp.c src/dsp/tone.c src/dsp/cab.c src/dsp/drums.c)
# The stock amp's filter chains are ill-conditioned: parity needs the stock's
# separate multiply and add, no FMA contraction (tests/test_stock_dsp_parity.py).
$(STOCK_OBJS) $(TARGET)/$(DSP_SRC)/FilteringFunctions/FilteringFunctions.c.o: \
  CFLAGS += -ffp-contract=off

# Hot and cold code (docs/FIRMWARE_BRINGUP.md, "Hot and cold code").
# ITCM is full, so the app runs code that is not on the audio path in place
# from flash (XIP): linker.ld .xiptext, in the slot data area (F:0x41000).
# Two rules, both checked by firmware/tools/hot_path.py
# (tests/test_audio_image.py):
#  - audio: nothing reachable from an ISR, from engine_task or
#    usb_audio_task, or through a driver callback is here;
#  - flash writes: nothing that runs while the flash is busy or after an app
#    update has erased this very code is here: the flash write path
#    (selfupdate.c: fw_begin, fw_rx_task, flash_store, the update session;
#    flash_rmw.c and the audio pump it runs while the flash is busy)
#    and what it reaches (tud_task and the USB class drivers, the CDC log,
#    the USB descriptors, fsl_flexspi.c).
# Every other file stays in ITCM. The main loop (main.c), boot (startup.c),
# the fault path (vectors.c, recovery.c) and crc32.c stay in ITCM on purpose.
# Recovery has no XIP code: it runs everything from ITCM.
COLD_SRC := \
  src/debug/console.c src/debug/regs.c \
  src/led.c src/cpu_power.c \
  src/ui/ui.c src/ui/display.c src/ui/controls.c src/ui/power.c src/ui/power_logic.c src/ui/lightbar.c \
  src/preset/preset.c src/preset/preset_check.c src/proto/proto.c \
  src/irstore/irstore.c src/irstore/irstore_port.c \
  src/audio/i2c_probe.c src/audio/frontend.c src/dsp/stock_data.c \
  $(BOARD_DIR)/board/clock_config.c \
  $(BOARD_DIR)/board/pin_mux.c \
  $(MCU_DIR)/drivers/fsl_clock.c \
  $(MCUX_CORE)/drivers/common/fsl_common.c \
  $(MCUX_CORE)/drivers/common/fsl_common_arm.c \
  $(MCUX_CORE)/drivers/igpio/fsl_gpio.c \
  $(MCUX_CORE)/drivers/ocotp/fsl_ocotp.c \
  $(MCUX_CORE)/drivers/adc_12b1msps_sar/fsl_adc.c \
  $(MCUX_CORE)/drivers/flexio/fsl_flexio.c \
  $(MCUX_CORE)/drivers/flexio/uart/fsl_flexio_uart.c \
  $(MCUX_CORE)/drivers/dmamux/fsl_dmamux.c
# tests/test_audio_image.py: negative controls of hot_path.py
COLD_SRC += $(COLD_EXTRA)
ifeq ($(VARIANT),recovery)
COLD_SRC :=
endif

SRC_S := src/stage2.S
OBJS  := $(patsubst %,$(TARGET)/%.o,$(SRC_C) $(SRC_S))

.PHONY: all build clean layout deps

# Order-only: every object must wait for dependency fetching on a fresh tree,
# but a re-fetch must not force recompilation.
$(OBJS): | deps

all: build

build: deps $(TARGET).vectors.bin $(TARGET).blob.bin $(TARGET).copier.bin $(TARGET).dtcmdata.bin

$(TARGET).elf: $(OBJS) linker.ld $(TARGET)/cold.ld
	$(CC) $(OBJS) $(LDFLAGS) -lgcc -o $@

# The .xiptext input list for linker.ld (INCLUDE cold.ld), from COLD_SRC.
$(TARGET)/cold.ld: Makefile
	@mkdir -p $(dir $@)
	@printf '/* generated from COLD_SRC (Makefile) */\n' > $@
	@for f in $(COLD_SRC); do printf '*/%s.o(.text .text.* .rodata .rodata.*)\n' "$$f"; done >> $@

# .vectors goes to block 0 offset 0; .blob to offset 0x7d4 (LMA in linker.ld).
# .dtcmdata.bin is the slot's data blob: .xiptext + .ocramdata + .dtcmdata
# (contiguous LMAs).
$(TARGET).vectors.bin $(TARGET).blob.bin $(TARGET).copier.bin $(TARGET).dtcmdata.bin: $(TARGET).elf
	$(OBJCOPY) -O binary -j .vectors $< $(TARGET).vectors.bin
	$(OBJCOPY) -O binary -j .blob $< $(TARGET).blob.bin
	$(OBJCOPY) -O binary -j .copier $< $(TARGET).copier.bin
	$(OBJCOPY) -O binary -j .xiptext -j .ocramdata -j .dtcmdata $< $(TARGET).dtcmdata.bin

$(TARGET)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

layout: deps $(TARGET).elf
	$(NM) $(TARGET).elf | grep -E '(_estack|__bss_|__blob_end__|__xiptext_|__ocramdata_|__dtcmdata_|__itcm_bss_|__dtcm_lo_|__dtcm_hi_|__ocram_|__stack_limit__|stage2|app_main)' | sort > $(BUILD)/layout.txt
	cat $(BUILD)/layout.txt

clean:
	rm -rf $(BUILD)

deps: $(TUSB_DIR)/src/tusb.c $(SDK_STAMP) $(DSP_DIR)/Include/arm_math.h

$(DSP_DIR)/Include/arm_math.h: cmsis-dsp.lock
	@mkdir -p .deps
	curl -fsSL -o .deps/cmsis-dsp.tar.gz "$(CMSIS_DSP_URL)"
	echo "$(CMSIS_DSP_SHA256)  .deps/cmsis-dsp.tar.gz" | shasum -a 256 -c - || \
	  echo "$(CMSIS_DSP_SHA256)  .deps/cmsis-dsp.tar.gz" | sha256sum -c -
	@rm -rf $(DSP_DIR) .deps/cmsis-dsp.tmp && mkdir -p .deps/cmsis-dsp.tmp
	tar -xzf .deps/cmsis-dsp.tar.gz -C .deps/cmsis-dsp.tmp --strip-components=1 \
	  --exclude='*/Testing' --exclude='*/Documentation' --exclude='*/Examples' \
	  --exclude='*/ComputeLibrary' --exclude='*/dsppp' --exclude='*/PythonWrapper'
	mv .deps/cmsis-dsp.tmp $(DSP_DIR)
	@rm -f .deps/cmsis-dsp.tar.gz
	@touch $@

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
