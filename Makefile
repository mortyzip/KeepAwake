# Payload Manager reads the version from the filename, e.g. keepawake_ps5_v1.4.0.elf
VERSION := 1.4.0

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

# The control page is embedded into the payloads as a C array.
PAGE_H  := build/index_html.h
DEPS    := src/web.h $(PAGE_H)
DEFINES := -DKEEPAWAKE_VERSION=\"v$(VERSION)\" -Ibuild

# PS5: ELF for the ps5-payload-sdk ELF loader.
PS5_CC     := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CFLAGS := -Wall -Werror -O2 -lSceSystemService -lSceUserService $(DEFINES)

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

$(PAGE_H): src/index.html
	@mkdir -p build
	cd src && xxd -i index.html > ../$@

$(PS5_ELF): src/ps5.c $(DEPS)
	$(PS5_CC) $(PS5_CFLAGS) -o $@ src/ps5.c

$(PS4_BIN): src/ps4.c $(DEPS)
	@mkdir -p build
	$(PS4_CC) $(PS4_CFLAGS) -c -o build/ps4.o src/ps4.c
	$(PS4_CC) --target=x86_64-unknown-freebsd -c -o build/ps4_crt0.o $(LIBPS4)/crt0.s
	$(PS4_LD) -o build/ps4.elf build/ps4_crt0.o build/ps4.o $(PS4_LDFLAGS)
	$(PS4_OBJCOPY) -O binary build/ps4.elf $@

# Runs the PS5 code on this computer with the console calls stubbed out, for
# working on the control page: make host && ./build/keepawake_host
# Needs BSD network headers (struct if_data), so macOS or BSD only.
host: build/keepawake_host

build/keepawake_host: src/ps5.c tools/host_stubs.c $(DEPS)
	cc -Wall -Werror $(DEFINES) -DKEEPAWAKE_SETTINGS=\"build/keepawake.cfg\" -o $@ src/ps5.c tools/host_stubs.c

version:
	@echo $(VERSION)

clean:
	rm -rf build keepawake*.elf keepawake*.bin

test-ps5: $(PS5_ELF)
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

test-ps4: $(PS4_BIN)
	nc -w 1 $(PS4_HOST) $(PS4_PORT) < $<

quit-ps5:
	curl -fsS -X POST http://$(PS5_HOST):9031/quit

quit-ps4:
	curl -fsS -X POST http://$(PS4_HOST):9031/quit

.PHONY: all ps5 ps4 host version clean test-ps5 test-ps4 quit-ps5 quit-ps4
