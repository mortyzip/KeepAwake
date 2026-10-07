# KeepAwake

A PS5 and PS4 payload that stops the console from going into rest mode while it runs.

Every 10 seconds it calls `sceSystemServicePowerTick()`, which resets the idle timer for auto rest mode.

## Usage

| Console | File | Send to |
|---|---|---|
| PS5 | `keepawake_ps5_v<version>.elf` | ELF loader, port 9021 |
| PS4 | `keepawake_ps4_v<version>.bin` | GoldHEN BinLoader, port 9090 |

- **Enable:** send the payload. You'll see "Keep Awake v<version> enabled".
- **Disable:** send the payload again. You'll see "Keep Awake v<version> disabled". You can also connect to port 9031 on the console, for example with `make stop-ps5` or `nc -z <console-ip> 9031`.

Only one copy runs at a time. It uses TCP port 9031 to know whether it's already running.

## Building

The Makefile expects the SDKs next to this folder (`../ps5-payload-sdk` and `../ps4-payload-sdk`). Set `PS5_PAYLOAD_SDK` or `PS4SDK` to use them from somewhere else.

```sh
make                              # build both
make ps5                          # or just one
make ps4
make test-ps5 PS5_HOST=<ps5-ip>   # build and send
make test-ps4 PS4_HOST=<ps4-ip>
```

Both builds need LLVM (`clang`, `ld.lld`, `llvm-objcopy`). The Makefile uses Homebrew's LLVM automatically when it's installed. Otherwise, set `LLVM_CONFIG` to your `llvm-config`.

The version is set by `VERSION` in the [Makefile](Makefile). It goes into the filenames, where Payload Manager reads it, and into the toasts.

## Source

- [src/ps5.c](src/ps5.c): built with the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).
- [src/ps4.c](src/ps4.c): built against libPS4 from the [Scene-Collective ps4-payload-sdk](https://github.com/Scene-Collective/ps4-payload-sdk). libPS4 has no `select()`, so this version checks for a stop request once a second.
