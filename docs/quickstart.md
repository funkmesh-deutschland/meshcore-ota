# Quickstart

Seven steps from the download to the first update over the air. Details for every step are in the [README](../README.md#getting-started-from-download-to-the-first-update-over-the-air) and in [app.md](app.md).

You need a computer with Linux or Windows, a MeshCore companion on USB or Bluetooth, and a repeater based on the nRF52840 or the ESP32.

## 1. Download

Get the file for your system from the [releases](https://github.com/funkmesh-deutschland/meshcore-ota/releases) and put it into a folder of its own. On Linux:

```
mv meshcore-ota-linux meshcore-ota
chmod +x meshcore-ota
```

Building firmware needs an internet connection and Python 3.9 or newer. On Windows install Python from python.org.

## 2. Start

```
./meshcore-ota
```

The menu opens. The first start creates the folders and your key pair. Keep a copy of `keys/signing.key` in a safe place and never share it.

## 3. Build the firmware (menu item 3)

Choose the MeshCore version and your device, for example `xiao`. The first build takes a few minutes. The result is in `install/`, and the build in `firmware/` is the basis for all later updates. Keep that folder.

## 4. Install it on the repeater

This is needed once per repeater.

| Device | File in `install/` | How |
| --- | --- | --- |
| nRF52840 | `...-mcota.uf2` | press reset twice, copy the file onto the drive that appears |
| nRF52840 | `...-mcota.zip` | run `start ota` on the repeater, upload with the app "nRF Device Firmware Update" |
| ESP32 | `...-mcota-merged.bin` | write with an ESP flasher over USB, starting at address 0 |
| ESP32 | `...-mcota.bin` | run `start ota` on the repeater, upload at `http://192.168.4.1/update` |

Your public key is part of the firmware. Name, radio parameters and passwords are set as with any MeshCore firmware.

## 5. Connect the companion (menu item 6)

Select the port of the companion. The MeshCore app must not be connected to the companion at the same time.

## 6. Release versions (menu item 2)

The list shows the repeater versions of MeshCore. Entering the number of a version toggles it. Only versions marked "yes" are sent to your repeaters.

When MeshCore publishes a new version later, release it here. Nothing else is needed for an update.

A repeater always receives the next released version after the one it is running. If it is two versions behind, it gets them one after the other.

## 7. Run the dashboard (menu item 1)

The dashboard asks over the air which of your repeaters are present. Each one appears with its device type and its build. If a newer released version exists for a repeater, the dashboard builds the firmware, signs the update and sends it on its own.

Leave the dashboard running until the repeater shows the new build as confirmed. At 0.5 % airtime this takes several hours per repeater.

A changed preset under menu item 4 is sent the same way and is the quickest way to try out an update.

## Next

| Topic | File |
| --- | --- |
| Menu, commands, dashboard | [app.md](app.md) |
| Settings and presets | [settings.md](settings.md) |
| Keys and key change | [keys.md](keys.md) |
| Repeaters over hops, how updates spread | [concept.md](concept.md) |
