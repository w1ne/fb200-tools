# fb200-hello firmware build. Phase A (Task 4) builds only our own sources;
# Task 5 adds TinyUSB.
TARGET  := build/fb200-hello
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm

CPUFLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
CFLAGS   := $(CPUFLAGS) -O2 -g3 -ffreestanding -fno-builtin -fno-common \
            -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I src
LDFLAGS  := $(CPUFLAGS) -nostdlib -T linker.ld -Wl,-Map=$(TARGET).map,--cref

SRC_C := src/startup.c src/vectors.c src/main.c
SRC_S := src/boot_header.S
OBJS  := $(patsubst %,$(TARGET)/%.o,$(SRC_C) $(SRC_S))

.PHONY: all build clean layout deps

all: build

build: $(TARGET).bin

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

layout: $(TARGET).elf
	$(NM) $(TARGET).elf | grep -E '(_estack|__itcm_|__data_|__bss_|reset_stub|app_main)' | sort > build/layout.txt
	cat build/layout.txt

clean:
	rm -rf build

# TinyUSB fetching is added in Task 5.
deps:
	@echo "no dependencies yet"
