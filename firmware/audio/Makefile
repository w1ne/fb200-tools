# fb200-hello firmware build. Booting, linking and vectors are ours
# (linker.ld, src/startup.c, src/vectors.c); TinyUSB, its imxrt BSP and the
# MCUXpresso SDK driver subset are compiled directly from the pinned
# .deps/tinyusb tree, never from vendored copies.
# VARIANT=recovery builds the resident recovery image (block 0); the default
# builds the app for the slot. See docs/BOOTLOADER.md §4.
VARIANT      ?= app
TARGET       := build/fb200-$(VARIANT)
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
  -DI2C_RETRY_TIMES=100000
CFLAGS    := $(CPUFLAGS) -O2 -g3 -ffreestanding -fno-builtin -fno-common \
             -ffunction-sections -fdata-sections \
             -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
             -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
             $(if $(filter recovery,$(VARIANT)),-DFB200_RECOVERY) \
             $(TUSB_INC) $(TUSB_DEFS) -I src -I src/compat -I board -I . -MMD -MP
LDFLAGS   := $(CPUFLAGS) -nostdlib -Wl,--gc-sections -T linker.ld -Wl,-Map=$(TARGET).map,--cref

SRC_C := \
  src/startup.c src/vectors.c src/main.c src/memfuncs.c \
  src/system_clock.c src/usb_descriptors.c src/compat/stubs.c src/led.c \
  src/dsp/dsp.c src/dsp/math.c src/dsp/gain.c src/dsp/testgen.c \
  src/dsp/conv.c src/dsp/biquad.c src/dsp/detector.c src/dsp/gate.c src/dsp/comp.c \
  src/dsp/mod.c src/dsp/reverb.c \
  src/dsp/drums.c src/dsp/tuner.c \
  src/dsp/amp.c src/dsp/tone.c src/dsp/cab.c \
  src/debug/log_fmt.c src/debug/cdc_log.c src/debug/console.c \
  src/debug/selfupdate.c src/debug/recovery.c src/debug/regs.c \
  src/audio/i2c_probe.c \
  src/audio/usb_audio.c src/audio/engine.c src/audio/frontend.c src/audio/bt_audio.c \
  src/ui/pads.c src/ui/display.c src/ui/controls.c src/ui/power.c \
  src/ui/ui.c src/ui/rgb.c src/preset/preset.c src/bt/bt.c src/audio/codec.c \
  src/proto/proto.c src/proto/proto_port.c src/usb_hid.c \
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
SRC_C := $(filter-out src/led.c src/dsp/% src/audio/engine.c \
                    src/audio/usb_audio.c src/audio/codec.c src/audio/sai.c \
                    src/audio/drift.c src/audio/frontend.c src/audio/bt_audio.c src/ui/% src/preset/% src/bt/% src/proto/% \
                    $(MCUX_CORE)/drivers/adc_12b1msps_sar/% $(MCUX_CORE)/drivers/flexio/% \
                    $(MCUX_CORE)/drivers/sai/% $(MCUX_CORE)/drivers/edma/% \
                    $(MCUX_CORE)/drivers/dmamux/%,$(SRC_C))
endif

ifneq ($(VARIANT),recovery)
SRC_C += $(foreach g,$(DSP_GROUPS),$(DSP_SRC)/$(g)/$(g).c)
endif

# Stock data - amp models, cab IRs, tone-stack tables (src/dsp/amp.c, tone.c,
# cab.c) and drum rhythms (src/dsp/drums.c) - is vendor data: generated from
# the user's own stock .mr into build/, never committed. Without the .mr the
# modules pass audio through (cab flat) and the drum machine stays silent.
STOCK_MR    ?= ../../fb200-stock.mr
PY_UNICORN  ?= python3
STOCK_DSP_C := build/stock_dsp_data.c
HAVE_UNICORN := $(shell $(PY_UNICORN) -c "import unicorn" 2>/dev/null && echo yes)
STOCK_STATE := $(if $(and $(wildcard $(STOCK_MR)),$(HAVE_UNICORN)),on,off)
STOCK_OBJS  := $(patsubst %,$(TARGET)/%.o,src/dsp/amp.c src/dsp/tone.c src/dsp/cab.c src/dsp/drums.c)
STOCK_DRUMS_C := build/stock_drums_data.c
ifneq ($(VARIANT),recovery)
ifeq ($(STOCK_STATE),on)
SRC_C  += $(STOCK_DSP_C) $(STOCK_DRUMS_C)
CFLAGS += -DFB200_STOCK_DSP=1 -DFB200_STOCK_DRUMS
else
$(info NOTE: no stock data ($(if $(wildcard $(STOCK_MR)),'$(PY_UNICORN)' cannot import unicorn - set PY_UNICORN,$(STOCK_MR) not found)): amp passes audio through, cab is flat, drums silent)
endif
endif
# Rebuild the modules when the data appears or goes away (FB200_STOCK_DSP).
$(shell mkdir -p build && echo $(STOCK_STATE) | cmp -s - build/stock_dsp.state || \
  echo $(STOCK_STATE) > build/stock_dsp.state)
$(STOCK_OBJS): build/stock_dsp.state
build/stock_dsp.state:        # again after `make clean`
	@mkdir -p build && echo $(STOCK_STATE) > $@
# The stock amp's filter chains are ill-conditioned: parity needs the stock's
# separate multiply and add, no FMA contraction (tests/test_stock_dsp_parity.py).
$(STOCK_OBJS) $(TARGET)/$(DSP_SRC)/FilteringFunctions/FilteringFunctions.c.o: \
  CFLAGS += -ffp-contract=off

$(STOCK_DSP_C): $(STOCK_MR) ../tools/extract_stock_dsp.py
	@$(PY_UNICORN) -c "import unicorn" 2>/dev/null || { echo "ERROR: $(STOCK_MR) exists but \
	'$(PY_UNICORN)' cannot import unicorn; set PY_UNICORN to a Python with unicorn \
	(pip install unicorn) or move the .mr away to build without the stock data"; exit 1; }
	$(PY_UNICORN) ../tools/extract_stock_dsp.py $(STOCK_MR) -o $@

$(STOCK_DRUMS_C): $(STOCK_MR) ../tools/gen_stock_drums.py
	@mkdir -p $(dir $@)
	python3 ../tools/gen_stock_drums.py $(STOCK_MR) $@
SRC_S := src/stage2.S
OBJS  := $(patsubst %,$(TARGET)/%.o,$(SRC_C) $(SRC_S))

.PHONY: all build clean layout deps

# Order-only: every object must wait for dependency fetching on a fresh tree,
# but a re-fetch must not force recompilation.
$(OBJS): | deps

all: build

build: deps $(TARGET).vectors.bin $(TARGET).blob.bin $(TARGET).copier.bin $(TARGET).dtcmdata.bin

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -lgcc -o $@

# .vectors goes to block 0 offset 0; .blob to offset 0x7d4 (LMA in linker.ld).
$(TARGET).vectors.bin $(TARGET).blob.bin $(TARGET).copier.bin $(TARGET).dtcmdata.bin: $(TARGET).elf
	$(OBJCOPY) -O binary -j .vectors $< $(TARGET).vectors.bin
	$(OBJCOPY) -O binary -j .blob $< $(TARGET).blob.bin
	$(OBJCOPY) -O binary -j .copier $< $(TARGET).copier.bin
	$(OBJCOPY) -O binary -j .dtcmdata $< $(TARGET).dtcmdata.bin

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
