# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

# fb200-labwired-smoke: a bare-register image for the LabWired FB200 twin
# (labwired/smoke.yaml). No SDK, no dependencies beyond arm-none-eabi-gcc.
TARGET  := build/labwired-smoke
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
SIZE    := $(CROSS)size

CPUFLAGS := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
CFLAGS   := $(CPUFLAGS) -O2 -g3 -ffreestanding -fno-builtin -fno-common \
            -Wall -Wextra -Werror -std=gnu11
LDFLAGS  := $(CPUFLAGS) -nostdlib -T linker.ld -Wl,-Map=$(TARGET).map

.PHONY: all build clean

all: build

build: $(TARGET).elf

$(TARGET).elf: src/main.c linker.ld Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) src/main.c $(LDFLAGS) -o $@
	$(SIZE) $@

clean:
	rm -rf build
