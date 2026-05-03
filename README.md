# 64DOS

64DOS is a tiny public-domain DOS-style operating system for BIOS-booted
x86-64 PCs. It boots from a 1.44 MB FAT12 floppy image, switches from BIOS
real mode into long mode, and runs a native 64-bit kernel with a small
COMMAND-style shell.

The first release is intentionally small:

- BIOS floppy boot, not UEFI.
- Native x86-64 long-mode kernel.
- VGA text console with serial output mirrored to COM1.
- Serial input for automation and PS/2 keyboard input for manual use.
- Read-only FAT12 `A:\` filesystem from the RAM-loaded floppy image.
- Built-in virtual `RFS:\` repository view for demo content and metadata inspection.
- Built-in commands: `VER`, `HELP`, `DIR`/`LS`, `TYPE`/`CAT`, `DUMP`/`HEX`,
  `WC`, `RUN`, `SCRIPT`, `DATE`, `TIME`, `COLOR`, `PROMPT`, `PWD`, `CLS`, `MEM`/`INFO`,
  `ECHO`, `PAUSE`, `BEEP`, `REBOOT`, `RFSREFS`, `PKG`, `VCS`.

Legacy 16-bit DOS `.COM` and `.EXE` compatibility is out of scope for v1.

## Build

Required tools:

- NASM
- GCC or another freestanding-capable x86-64 C compiler
- GNU `ld` and `objcopy`
- Python 3
- QEMU for smoke tests
- mtools for compatibility checks

`make check-tools` validates build prerequisites only. Use `make check-tools-all` to also validate smoke-test and compatibility tooling.

On Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y nasm gcc binutils qemu-system-x86 mtools make python3
make check-tools          # build prerequisites
make check-tools-all      # build + smoke/compat prerequisites
make
make test
make smoke
```

The final image is written to:

```text
dist/64dos.img
```

It is always exactly `1,474,560` bytes.

## Run

Interactive (curses + serial mirrored to stdout):

```sh
qemu-system-x86_64 -drive file=dist/64dos.img,format=raw,if=floppy -boot a -serial stdio -display curses
```

CI-style serial-only boot:

```sh
python3 scripts/qemu-smoke.py dist/64dos.img
```

## RFS Notes

64DOS boots with a DOS-style `RFS:\>` prompt. In this build, `RFS:` is an in-memory, read-only virtual repository namespace (separate from FAT12 `A:`). You can inspect it with `DIR`, `TYPE`, and `RFSREFS`.

## Script Language

Use `SCRIPT filename` to execute line-based instructions from a FAT12 root file at runtime.

Supported script instructions:

- `SET`
- `ADD`
- `PRINT`
- `RUN`

## Layout

- `boot/`: BIOS stage1 and stage2 bootloader sources.
- `kernel/`: 64-bit freestanding kernel and internal ABI.
- `image_root/`: files copied into the FAT12 root directory.
- `scripts/`: reproducible image builder and validation tools.
- `tests/`: host-side FAT12 tests.
- `upstream/pdos/`: PDOS provenance, pinned to the source used for design.

## Public Domain

64DOS is released under the Unlicense. PDOS provenance is documented in
`UPSTREAM.md`; this repository does not import GPL, FreeDOS, or Microsoft
MS-DOS code.
