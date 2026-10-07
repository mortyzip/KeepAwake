# Payload Manager reads the version from the filename, e.g. keepawake_ps5_v1.0.0.elf
VERSION := 1.0.0

PS5_PAYLOAD_SDK ?= $(abspath ../ps5-payload-sdk)
PS4SDK          ?= $(abspath ../ps4-payload-sdk)

PS5_HOST ?= ps5
PS5_PORT ?= 9021
PS4_HOST ?= ps4
PS4_PORT ?= 9090

# The SDKs need an LLVM toolchain; use Homebrew's when it isn't on PATH.
ifneq ($(wildcard /opt/homebrew/opt/llvm/bin/llvm-config),)
    export LLVM_CONFIG ?= /opt/homebrew/opt/llvm/bin/llvm-config
endif
LLVM_BIN := $(shell $(or $(LLVM_CONFIG),llvm-config) --bindir 2>/dev/null)
LLVM_PFX := $(if $(LLVM_BIN),$(LLVM_BIN)/)

PS5_ELF := keepawake_ps5_v$(VERSION).elf
PS4_BIN := keepawake_ps4_v$(VERSION).bin

DEFINES := -DKEEPAWAKE_VERSION=\"v$(VERSION)\"

# PS5: ELF for the ps5-payload-sdk ELF loader.
PS5_CC     := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CFLAGS := -Wall -Werror -O2 -lSceSystemService $(DEFINES)

# PS4: raw binary for GoldHEN's BinLoader, linked against libPS4.
LIBPS4       := $(PS4SDK)/libPS4
PS4_CC       := $(LLVM_PFX)clang
PS4_LD       := $(or $(wildcard $(LLVM_PFX)ld.lld),ld.lld)
PS4_OBJCOPY  := $(LLVM_PFX)llvm-objcopy
PS4_CFLAGS   := --target=x86_64-unknown-freebsd -I$(LIBPS4)/include $(DEFINES) \
                -Os -std=c11 -ffreestanding -fno-builtin -nostdlib -fPIE \
                -ffunction-sections -fdata-sections -masm=intel -march=btver2 \
                -Wall -Werror
PS4_LDFLAGS  := -T $(LIBPS4)/linker.x --gc-sections --build-id=none \
                -L$(LIBPS4) -lPS4

all: ps5 ps4

ps5: $(PS5_ELF)
ps4: $(PS4_BIN)

$(PS5_ELF): src/ps5.c
	$(PS5_CC) $(PS5_CFLAGS) -o $@ $^

$(PS4_BIN): src/ps4.c
	$(PS4_CC) $(PS4_CFLAGS) -c -o ps4.o src/ps4.c
	$(PS4_CC) --target=x86_64-unknown-freebsd -c -o ps4_crt0.o $(LIBPS4)/crt0.s
	$(PS4_LD) -o ps4.elf ps4_crt0.o ps4.o $(PS4_LDFLAGS)
	$(PS4_OBJCOPY) -O binary ps4.elf $@
	rm -f ps4.o ps4_crt0.o ps4.elf

version:
	@echo $(VERSION)

clean:
	rm -f keepawake*.elf keepawake*.bin ps4.o ps4_crt0.o ps4.elf

test-ps5: $(PS5_ELF)
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $^

test-ps4: $(PS4_BIN)
	nc -w 1 $(PS4_HOST) $(PS4_PORT) < $^

stop-ps5:
	nc -z $(PS5_HOST) 9031

stop-ps4:
	nc -z $(PS4_HOST) 9031

.PHONY: all ps5 ps4 version clean test-ps5 test-ps4 stop-ps5 stop-ps4
