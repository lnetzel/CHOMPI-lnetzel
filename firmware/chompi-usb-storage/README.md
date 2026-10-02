# CHOMPI — USB Storage Firmware

Utility firmware that makes CHOMPI's microSD card show up on a computer as a normal USB drive,
so you can do file management with a need for external SD card reader. Not, this is NOT an instrument and does nto aim to. But in combination with the [chompi-launcher](https://github.com/sfaber02/CHOMPI/tree/main/firmware/chompi-launcher) firmware this is highly usable to switch quick between file management and other instrument firmwares.

---

## How it works

CHOMPI acts as a USB Mass Storage (Bulk-Only, SCSI) device on its USB-C port, so Windows and
macOS use their built-in drivers and no installation is needed. The card is exposed block by
block, so it mounts like any card reader: a FAT volume gets a drive letter / desktop icon, and
an unformatted card appears as a raw disk the computer can format.

Verified on Windows. macOS uses the same standard class driver but has not been tested.

## Using it

1. Build `CHOMPI.bin` (see [Building on Windows](#building-on-windows)) and put it on the SD card,
   with any other `.bin` removed. Power on CHOMPI; the slow rainbow LED pattern shows the
   bootloader installing it.
2. Wait for the LEDs to turn **green**, then connect the USB-C port to the computer.
3. The card appears as a drive named `CHOMPI-SD`.
4. **Eject** the drive in the operating system before unplugging the cable or restarting CHOMPI.

To return to the instrument, put another firmware's `.bin` on the card, eject, and restart.

While connected the computer owns the card. CHOMPI itself never touches it, so don't power up
another firmware with the card mounted.

The name shown on the computer is the volume label stored on the card. This firmware sets it to
`CHOMPI-SD` at every boot; to change it, edit `f_setlabel` in `code/src/main.cpp` (FAT labels
allow up to 11 characters). The capacity shown is whatever the card reports.

## LED status

| Colour | Meaning |
|---|---|
| Amber | Starting up |
| Magenta / yellow | Writing / flushing the boot log |
| Cyan | Starting USB |
| **Green** | Ready, connect to a computer |
| Red | SD card or filesystem problem |
| Blue | USB failed to start |

Green only means the firmware is ready; it does not confirm that a computer has mounted the drive.

## Boot log

Each boot appends to `CHOMPI_USB.LOG` in the card's root (when a FAT volume can be mounted):
card size, sector-0 read, FAT mount and label results, and the USB start stage. Values are
FatFs result codes where `0` means success. The log is written before USB starts, never while
the computer has the card. If the volume can't be mounted there is no log; use the LED colour.

## Building on Windows

You need:

- **MSYS2** (https://www.msys2.org) for a shell and GNU Make.
- **Arm GNU Toolchain 10.3-2021.10** (`arm-none-eabi-gcc` 10.3.1), installed to
  `C:\armgnu\10.3-2021.10`. Use exactly this version: newer compilers can cause SD communication
  problems, and the Makefile refuses to build with any other.
- The `chompi-wave` and `chompi-tape` folders next to this one. This project reuses their
  libDaisy and LED helper sources.

Open an **MSYS2 UCRT64** shell, then install Make once:

```sh
pacman -S --needed make
```

In each new shell, put the toolchain on the `PATH`, then build from `code/src`:

```sh
export PATH="/c/armgnu/10.3-2021.10/bin:$PATH"
cd /path/to/firmware/chompi-usb-storage/code/src
make check-toolchain   # prints GCC 10.3.1 if the toolchain is right
make -j4
```

The output is `code/src/build/CHOMPI.bin`. The build links the prebuilt
`../chompi-wave/code/libs/libDaisy/build/libdaisy.a`; if that file is missing, run `make` once in
`../chompi-wave/code/libs/libDaisy` first.

To install it, copy `CHOMPI.bin` to the root of the SD card (deleting any other `.bin`), put
the card in CHOMPI and power on. If the board has never had the CHOMPI bootloader, install it
first as described in the [bootloader guide](../chompi-bootloader-v6.4-beta/README.md).

## Layout

```
code/src/main.cpp       startup, SD init, boot log
code/src/usb_msc.cpp    USB device and SCSI mass-storage handling
code/src/config/        FatFs options (volume label support)
```

It reuses libDaisy and the USB device stack from `../chompi-wave/code/libs/` and the LED helper
from `../chompi-tape/code/src/`, so those folders must be present.

## Notes

- USB IDs are VID `0x1209` / PID `0xC0A1`. Replace them with assigned IDs before wide distribution.
- Disk reads and writes use the same DMA path as FatFs. The SD driver's polled mode failed under
  USB interrupt load, so don't switch back to it.

## License

MIT — see [`LICENSE`](../../LICENSE). [`THIRD_PARTY.md`](../../THIRD_PARTY.md) lists the work this
builds on. The CHOMPI name and marks are not covered by the license — see
[`TRADEMARKS.md`](../../TRADEMARKS.md).
