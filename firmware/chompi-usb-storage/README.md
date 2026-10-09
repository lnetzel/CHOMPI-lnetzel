# CHOMPI — USB Storage Firmware v1.5

Utility firmware that makes CHOMPI's microSD card show up on a computer as a normal USB drive,
so you can do file management with a need for external SD card reader. Not, this is NOT an instrument and does not aim to be: However; In combination with the [Multi-Firmware Launcher](https://github.com/sfaber02/CHOMPI/releases#release-launcher-v1.1) firmware this is practical to switch quick between file management and other instrument firmwares.

---

## How it works

CHOMPI acts as a USB Mass Storage (Bulk-Only, SCSI) device on its USB-C port, so Windows and
macOS use their built-in drivers and no installation is needed. The card is exposed block by
block, so it mounts like any card reader: a FAT volume gets a drive letter / desktop icon, and
an unformatted card appears as a raw disk the computer can format.

Verified on Windows. macOS uses the same standard class driver but has not been tested.

## Using it

1. Builds `chompi_usb_storage_v1.5.bin` (see [Building on Windows](#building-on-windows)) and put it on the SD card,
   with any other `.bin` removed. Power on CHOMPI; the slow rainbow LED pattern shows the
   bootloader installing it.
2. Wait for the LEDs to turn **green**, then connect the USB-C port to the computer.
3. The card appears as a drive named `CHOMPI-SD`.
4. **Eject** the drive in the operating system before unplugging the cable. CHOMPI then
   restarts (see [Restarting](#restarting)).

## Disk usage bar

At startup, the lower row of 15 keyboard keys sweeps up in blue from left to right and
settles on how much of the card's file system is used: an empty card leaves the row dark,
a half-full card lights 7 keys fully and the 8th half, and a nearly full card lights all 15.
After the sweep the bar stays on and pulses gently (between about 75 % and full brightness).

The measurement reads the FAT free-cluster count at boot, so it matches what the computer
reports for the `CHOMPI-SD` drive — as long as the drive was ejected properly after the
last file management session. If the card's free-space info is missing or cannot be read,
the bar simply stays off and the row shows the normal status colour instead.

## Restarting

While the drive is ready, the **overdub** key glows red. Press it once to arm a restart: the
key starts blinking ("are you sure?") and the **chompi** key turns red. Press the chompi key
to confirm — the drive is detached from the computer, the green LEDs quickly fade to white,
and CHOMPI restarts (back through the bootloader, e.g. into the Multi-Firmware Launcher).
Press the blinking overdub key again to cancel; nothing changes.

The overdub key glows red in every LED status — even on an SD or USB error — so the device
can always be restarted without power cycling.

**Ejecting** the drive on the computer restarts CHOMPI the same way, a quarter of a second
later. With the [Multi-Firmware Launcher](https://github.com/sfaber02/CHOMPI/releases), that
brings the picker back without touching CHOMPI, so a script can copy files and return to an
instrument firmware on its own (on Linux: `eject /dev/sdX`). Only a real eject does this;
unmounting alone leaves the drive attached.

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
| **Green** | Ready, connect to a computer (overdub key is red: restart trigger) |
| Red | SD card or filesystem problem |
| Blue | USB failed to start |

The status colours cover all keys except the lower row once the
[disk usage bar](#disk-usage-bar) has taken it over; that row pulses blue instead.

Green only means the firmware is ready; it does not confirm that a computer has mounted the drive.

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

The output is `code/src/build/chompi_usb_storage_v1.5.bin`. The build links the prebuilt
`../chompi-wave/code/libs/libDaisy/build/libdaisy.a`; if that file is missing, run `make` once in
`../chompi-wave/code/libs/libDaisy` first.

To install it, copy `chompi_usb_storage_v1.5.bin` to the root of the SD card (deleting any other `.bin`), put
the card in CHOMPI and power on. If the board has never had the CHOMPI bootloader, install it
first as described in the [bootloader guide](../chompi-bootloader-v6.4-beta/README.md).

## Layout

```
code/src/main.cpp       startup, SD init, boot log, disk-usage bar
code/src/usb_msc.cpp    USB device and SCSI mass-storage handling
code/src/config/        FatFs options (volume label support)
```

It reuses libDaisy and the USB device stack from `../chompi-wave/code/libs/` and the LED helper
from `../chompi-tape/code/src/`, so those folders must be present.

## Notes

- USB IDs are VID `0x1209` / PID `0xC0A1`. Replace them with assigned IDs before wide distribution.
- Disk reads and writes at the max USB speed CHOMPI hardware allows. 
  - Write ~ 0.81 MB/s
  - Read ~ 1.03 MB/s

## License

MIT — see [`LICENSE`](../../LICENSE). [`THIRD_PARTY.md`](../../THIRD_PARTY.md) lists the work this
builds on. The CHOMPI name and marks are not covered by the license — see
[`TRADEMARKS.md`](../../TRADEMARKS.md).
