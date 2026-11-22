# Device classes

MeshCore-OTA knows three ways of activating a new firmware on a repeater. Which one applies depends on the memory of the device. Radio protocol, package format and checks are the same in all cases.

## Support status

```
meshcore-ota devices
```

lists all devices that MeshCore knows as repeaters in the selected version and classifies them:

| Display | Meaning | Devices |
| --- | --- | --- |
| `yes` | supported | repeaters with nRF52840 and the memory layout for SoftDevice S140 v6 or v7 (for example Seeed Xiao nRF52840, RAK4631, Heltec T114, ProMicro), ESP32 repeaters (for example Heltec V3 and V4) |
| `no` | not supported | nRF52840 with a custom memory layout, RP2040, STM32 and other processors |

## Overview

| Device class | Example | Where the new firmware is assembled | Switch | Rollback |
| --- | --- | --- | --- | --- |
| nRF52840 with external flash | Seeed Xiao nRF52840 | complete in the external flash | loader swaps page by page | yes |
| nRF52840 without external flash | RAK4631 | only the received blocks are stored, the loader builds from them | loader, in place | yes, as long as there is enough room |
| ESP32 with two app partitions | Heltec V3, Heltec V4 | complete in the second app partition | changing the boot partition, no separate loader | yes, the old partition is left in place |

The first two classes share the port in `firmware/mcota/McotaPort_nrf52.cpp`, the ESP32 has `firmware/mcota/McotaPort_esp32.cpp`. Commands, keys, presets, lock and the trial boot sequence are implemented once in `firmware/mcota/McotaService.cpp`.

## nRF52840

### Memory layout

The bootloader of the device, the SoftDevice and the MeshCore file system with the settings remain untouched. In between are (addresses in the internal flash):

| Area | Xiao nRF52840 | S140 v6 without external flash | S140 v7 without external flash |
| --- | --- | --- | --- |
| Loader | `0x27000`, 1 page | `0x26000`, 3 pages | `0x27000`, 3 pages |
| Firmware (slot A) | from `0x28000`, 194 pages (776 KB) | from `0x29000`, 193 pages (772 KB) | from `0x2A000`, 192 pages (768 KB) |
| Status and journal | `0xEA000`, 2 pages | `0xEA000`, 2 pages | `0xEA000`, 2 pages |
| Keys, preset, lock | `0xEC000`, 1 page | `0xEC000`, 1 page | `0xEC000`, 1 page |

On the Xiao, the external flash on the board (2 MB) holds the new firmware (slot B), the scratch page for the swap and the package store. On devices without external flash, firmware and package store share slot A: the package is stored in the free space behind the running firmware.

`meshcore-ota` recognises the layout of an nRF52840 device by the linker script of its MeshCore environment (`nrf52840_s140_v6.ld` or `nrf52840_s140_v7.ld`). It does not build devices with a different linker script.

### With external flash

The repeater writes every finished page to slot B. To apply, the loader swaps both slots page by page, with a journal. Afterwards the old firmware is in slot B and serves as the rollback copy. The package stays in the package store and is passed on after the update. Details on the loader are in [concept.md](concept.md).

### Without external flash

There is no room for two complete firmware images. The repeater therefore stores only the blocks that arrive over the air. It applies each block in RAM on reception and checks the result against the signed manifest. The running firmware remains untouched in the process.

On reboot the loader plans first and does not change anything yet. It chooses the first method that fits:

1. **With rollback copy, package is kept.** The loader compresses every page that changes and places it behind the package. Then it builds the new page from its block. It does not touch unchanged pages. After the update the repeater can pass the package on.
2. **With rollback copy, package is consumed.** If the free space is not sufficient, the loader uses the space of the blocks already applied for the rollback copy. After that the package is gone. The repeater therefore waits before applying until no neighbour has loaded from it for 24 hours.
3. **In place without rollback copy.** If that does not fit either, the loader rebuilds page by page and only backs up the page it is currently working on. A power failure does no harm, but there is no way back to the old firmware, and there is no trial boot.

If there is no room for a rollback copy, the repeater does not apply on its own. It keeps the update and waits for `ota apply`, over USB or through admin access over the air. `ota info` then shows `no-rollback awaiting-command`. It only proceeds on its own if the signed package allows it (`meshcore-ota update --no-rollback`).

If none of the methods fits, the loader rejects the job and the old firmware keeps running.

Which method applies depends on how many pages change and how much space is free behind the firmware. An update that changes almost every page (for example after a compiler change) is only possible through the third method on a large firmware.

Every step is recorded in the journal. The simulation (`sim/test_compact.c`) interrupts all three methods at every single write and erase point, 7,489 points in total.

## ESP32

### Memory layout

MeshCore-OTA does not change the partition table of MeshCore and needs exactly two app partitions (OTA partitions). If the second one is missing, the build aborts with a message.

| What | Where |
| --- | --- |
| running firmware | the app partition that was booted from |
| new firmware | the other app partition, from its start |
| package for passing on | the end of the other app partition |
| status of the update | NVS, namespace `meshcore-ota`, entry `status` |
| keys, preset, lock | NVS, namespace `meshcore-ota`, entry `local` |

Everything MeshCore-OTA writes raw is in the other app partition, never in the running one.

For ESP32 the core is configured for at most 448 pages of firmware (`MCOTA_MAX_PAGES`, 1,792 KB). How much of the end of an app partition remains for the package depends on its size (at least 16 KB, at most 1 MB):

| Size of the app partition | Space for the firmware | Space for the package |
| --- | --- | --- |
| 1.25 MB | 1,264 KB | 16 KB |
| 1.875 MB | 1,792 KB | 128 KB |
| 2.75 MB and more | 1,792 KB | 1 MB |

With small app partitions only a small package fits into the store. Such a repeater can apply a larger update itself but cannot pass it on to neighbours (`ota info` shows `no-sharing`). If the firmware grows beyond the available space, the build aborts with a message.

After a confirmed update the package is at the end of the partition that is now running. The new firmware copies it once to the end of the other partition and records that in NVS (entry `mirror`). From there it passes the package on.

### Switch, trial boot and rollback

1. `ota apply` or applying on its own writes the job to the status and reboots.
2. On start the old firmware verifies the checksums of both partitions, records the switch, sets the other partition as the boot partition and reboots.
3. The new firmware records the trial boot. Confirmation works as on the nRF52840: after 1 minute of runtime at the earliest and only if a neighbour has replied. Without confirmation it reboots after 10 minutes, up to three times.
4. On the next start without confirmation it sets the old partition as the boot partition again and reboots. The old firmware records the rollback, and the update counts as failed.

If the old firmware is running again after a recorded switch because the Espressif bootloader has switched back by itself, the core also records the rollback and does not switch again. The firmware only reports itself as valid to the bootloader after MeshCore-OTA has started and recorded the trial boot.

### First installation

`meshcore-ota install Heltec_v3_repeater` stores two files in `install/`:

- `...-mcota-merged.bin`: bootloader, partition table and firmware in one image, for an ESP flasher over USB starting at address 0.
- `...-mcota.bin`: the firmware only, for the Wi-Fi update of MeshCore (`start ota`).

The file in `firmware/` is the same as `...-mcota.bin`. The repeater reports the size and checksum of exactly this image, which is how the program identifies the build.

### Limitations on ESP32

- The prediction of shifted addresses exists for ARM (nRF52840) and Xtensa (ESP32, ESP32-S3). For other instruction sets such as RISC-V (ESP32-C3, ESP32-C6) the program creates the patch without it, and updates are larger there.
- The program does not pin the compiler for ESP32. If MeshCore changes the Espressif platform, the next update will be large.
- The fixed order in memory is established by `firmware/esp32/link_order.py`, an extra script for PlatformIO that is hooked into every ESP32 build.

## What determines the size of an update

- **Unchanged pages cost nothing.** They appear in the manifest as a single bit.
- **Prediction of shifted addresses.** The package carries a table stating which sections have shifted and by how much. The repeater uses it to convert branch targets and stored addresses of the old firmware before it applies the difference.
- **Sparse encoding.** A block only lists the bytes that differ and is compressed with Deflate.
- **Same compiler.** If the compiler changes, almost every function changes, even with identical source code.
- **Bundles.** Several small blocks travel together in one radio packet with up to 152 bytes of payload.

The actual number of radio packets of a package is shown by the dashboard and by `meshcore-ota info <package>`.
