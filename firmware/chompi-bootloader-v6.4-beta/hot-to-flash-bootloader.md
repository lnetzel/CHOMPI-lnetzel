# Flash the CHOMPI v6.4 Beta Bootloader on Windows

This is the STM32CubeProgrammer procedure used successfully on both CHOMPIs
on 2026-10-25. It installs the official CHOMPI v6.4 beta bootloader, not an
application such as TAPE or PIPES. No source build is required.

## Prerequisites and Paths

- Windows PowerShell, including the terminal in VS Code. Run the commands below
  in the same PowerShell terminal, in order. The working directory does not matter.
- A USB cable known to transfer data, connected directly to the Daisy Seed's
  own USB socket inside the CHOMPI, not just an external power socket.
- Access to the Daisy Seed's BOOT and RESET buttons. These are not the CHOMPI
  keyboard or transport buttons. Avoid touching exposed circuitry with metal.
- STM32CubeProgrammer. Version 2.16.0 was already installed on this computer:
  `C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe`.
  On another computer, install it from
  <https://www.st.com/en/development-tools/stm32cubeprog.html> first.
  Complete any installer elevation prompts yourself.
- A working STM32 USB DFU driver. This computer already had the
  STMicroelectronics WinUSB driver, version 1.1.0.0. No driver installation or
  replacement was needed. If CubeProgrammer cannot access an enumerated DFU
  device, consult its USB driver installation instructions.
- Internet access to download the official release:
  <https://github.com/CHOMPI-Club/CHOMPI/releases/download/chompi-open-source-bundle/chompi_bootloader_v6_4.bin>.

The repository's bootloader source is in
[firmware/chompi-bootloader-v6.4-beta](firmware/chompi-bootloader-v6.4-beta/README.md).
The release binary and programmer are not supplied by that source folder.
Neither `dfu-util`, an ARM compiler, MSYS2, nor an ST-Link is needed for this
procedure. We used CubeProgrammer instead of the README's `dfu-util` command.

The commands below store the binary and backups in
`$env:USERPROFILE\Downloads\CHOMPI-Bootloader`, which on this computer is
`C:\Users\LarsNetzel\Downloads\CHOMPI-Bootloader`.
In the original session we used `$env:TEMP` instead
(`C:\Users\LarsNetzel\AppData\Local\Temp`); this guide uses Downloads so
backups are less likely to be automatically deleted.

## Numbered Procedure

1. **Start with the CHOMPI off and disconnected.**

   Disconnect its USB cable and external power cable, and switch the CHOMPI
   off. If it has a battery, make sure it is switched off too. Disconnect any
   other CHOMPI from the computer so only the intended unit can be flashed.

   Optional precaution: remove the SD card while powered off. Flashing the
   bootloader does not access the card, but removing it prevents an application
   `.bin` on the card from being installed when CHOMPI subsequently starts.

2. **Set the tool and file paths in PowerShell. Keep the CHOMPI off for now.**

   ```powershell
   $programmer = 'C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
   $workDir = Join-Path $env:USERPROFILE 'Downloads\CHOMPI-Bootloader'
   $image = Join-Path $workDir 'chompi_bootloader_v6_4.bin'
   $expectedMd5 = '580B187FEC405849FB401EB699281E4C'
   if (-not (Test-Path -LiteralPath $programmer)) { throw 'Install STM32CubeProgrammer or correct $programmer before continuing.' }
   New-Item -ItemType Directory -Path $workDir -Force -ErrorAction Stop | Out-Null
   ```

   These commands select the installed programmer and create the download and
   backup directory. They do not communicate with or modify the CHOMPI.

3. **Download and verify the official bootloader.**

   ```powershell
   $releaseUrl = 'https://github.com/CHOMPI-Club/CHOMPI/releases/download/chompi-open-source-bundle/chompi_bootloader_v6_4.bin'
   Invoke-WebRequest -UseBasicParsing -Uri $releaseUrl -OutFile $image -ErrorAction Stop
   Get-Item -LiteralPath $image | Select-Object FullName,Length
   Get-FileHash -Algorithm MD5 -LiteralPath $image
   if ((Get-Item -LiteralPath $image).Length -ne 119612 -or (Get-FileHash -Algorithm MD5 -LiteralPath $image).Hash -ne $expectedMd5) { throw 'Wrong bootloader image. Stop; do not flash.' }
   ```

   The required size is **119,612 bytes** and the required MD5 is
   **580b187fec405849fb401eb699281e4c**. Letter case does not matter.
   These match the repository README and the image used on both CHOMPIs.
   If any command fails, stop and resolve it before continuing.

4. **Connect the intended CHOMPI and enter DFU mode.**

   Connect USB from the computer directly to the Daisy Seed's USB socket.
   If needed for your normal setup, connect external power and switch CHOMPI
   on. USB can power the Seed even while the CHOMPI's main switch is off;
   check the Seed's power LED rather than assuming the switch removes USB power.

   With the Seed powered and USB connected, hold **BOOT**, press and release
   **RESET** while still holding BOOT, then release **BOOT**. Do not keep either
   button held during the following commands. A Windows USB sound may occur,
   but the device listing in the next step is the actual confirmation.

   Keep USB and any power supply connected and stable throughout backup,
   programming, and verification. Do not unplug or reset during those operations.

5. **Confirm that CubeProgrammer sees exactly one DFU device.**

   ```powershell
   & $programmer -l usb
   ```

   We saw `Total number of available STM32 device in DFU mode: 1`, device index
   `USB1`, product `DFU in FS Mode`, device ID `0x0450`, and serial number
   `200364500000`. The USB hardware ID was `0483:DF11`.

   If nothing is listed, do not flash. Check the data cable and direct USB
   connection, then repeat BOOT + RESET. If more than one device is listed,
   disconnect the others before continuing.

   Set the following values from **your current listing**:

   ```powershell
   $usbPort = 'USB1'
   $serial = '200364500000'
   ```

   Both CHOMPIs in our session reported this same serial, so it must not be
   treated as proof of which physical CHOMPI is connected. Connect only one.

6. **Back up the existing internal flash before erasing anything.**

   ```powershell
   $backup = Join-Path $workDir ('chompi_bootloader_backup_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff') + '_' + $serial + '.bin')
   & $programmer -c "port=$usbPort" "sn=$serial" -u 0x08000000 0x20000 $backup
   if ($LASTEXITCODE -ne 0) { throw 'Backup failed. Stop; do not flash.' }
   if ((Get-Item -LiteralPath $backup -ErrorAction Stop).Length -ne 131072) { throw 'Unexpected backup size. Stop; do not flash.' }
   Write-Output "Backup saved to: $backup"
   ```

   `-c` connects to the chosen DFU device. `-u` reads device memory into a
   computer file; it does not program flash. The address `0x08000000` is the
   start of internal flash and `0x20000` is 128 KiB (131,072 bytes), covering
   the STM32H750's internal bootloader flash. Expect `Data read successfully`.
   Each backup has a new timestamp; keep it for recovery.

7. **Recheck the image, then flash v6.4 and verify it.**

   ```powershell
   if ((Get-Item -LiteralPath $image).Length -ne 119612 -or (Get-FileHash -Algorithm MD5 -LiteralPath $image).Hash -ne $expectedMd5) { throw 'Bootloader verification failed; refusing to flash.' }
   & $programmer -c "port=$usbPort" "sn=$serial" -w $image 0x08000000 -v
   if ($LASTEXITCODE -ne 0) { throw 'Programming or verification failed. Keep the device connected and investigate before assuming success.' }
   ```

   **This is the command that changes the device.** `-w` writes the binary at
   `0x08000000`; CubeProgrammer automatically erases the corresponding internal
   flash sector first. `-v` reads back the programmed contents and compares them
   with the image. No separate erase command is needed. Do not use mass erase
   or change the address.

   Wait until the command finishes. Our successful output included:

   ```text
   Erasing internal memory sector 0
   File download complete
   Download verified successfully
   ```

   The final verification message, not a USB sound or LED pattern, confirms
   installation. These commands do not write the external QSPI application
   flash or the SD card.

8. **Start CHOMPI normally after verification has finished.**

   If CHOMPI has already restarted and works, no button press is necessary.
   Otherwise, make sure BOOT is released and press and release RESET once.
   Do not hold BOOT for normal startup. We did not send a separate software
   reset/start command, so do not assume that programming always restarts it.

   If you removed the SD card, first disconnect USB and external power and
   switch CHOMPI off. Reinsert the card, then reconnect the power normally used
   by CHOMPI and switch it on. The Seed's programming USB cable is no longer
   required. A firmware `.bin` on the SD card may trigger the normal application
   update on startup; that is separate from this bootloader installation.

9. **Check normal operation and disconnect safely.**

   Confirm that the expected application starts and the controls and audio
   work. In our session both units programmed and verified successfully, and
   normal operation was confirmed afterward. Normal startup alone does not
   identify the bootloader version; the verified write is the version evidence.

   Once all programmer commands have finished, USB can be disconnected. If
   USB is the Seed's only power source, disconnecting it powers the Seed down.
   Keep your backup and the verified release image in the Downloads directory.

10. **For another CHOMPI, identify and back up that unit separately.**

    Disconnect the first unit's programming USB cable. Repeat steps 4 through 9
    with the next unit. Run the device listing again, update `$usbPort` and
    `$serial` if necessary, and run the backup command again to create a fresh
    timestamped backup. Never reuse a previous unit's backup as if it belonged
    to the new unit.