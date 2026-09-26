# libgm

C library that speaks the Groovy protocol designed by psakhis: sends native-resolution frames,
audio and mode changes over UDP to a CRT receiver, and receives gamepad input back.

## What this repository is

A public copy of the library as embedded in crt-bridge emitter releases; development happens
elsewhere. Each release of `crt-bridge/RetroArch` pins the exact commit it was built from, as a
git submodule, so the source behind any given release is always traceable from that release. This
repository receives one commit per synchronisation, never a history rewrite.

## Build

**Windows (MSYS2 MINGW64):**

```
make CC=gcc AR=ar libgm.a libgm.dll
```

**Linux:**

```
make libgm.a
```

`make` with no target, `make test` and `make seq-test` also work in this checkout alone. Two
optional test tools need sources that live outside this repository: `groovy_test_bridge.dll`
(the RetroArch driver headers, `EMITTER_SRC`) and `proto_probe.dll` (the receiver's protocol
parser, `RECEIVER_SRC`). When their sources are not found, `make` prints "skipped" for each and
carries on. Set those two variables to build them.

## License

GPL-3.0-or-later (see `LICENSE`). `src/lz4.c` and `src/lz4.h` are LZ4 by Yann Collet,
BSD-2-Clause — see their own headers and `src/LZ4-PROVENANCE.md`.
