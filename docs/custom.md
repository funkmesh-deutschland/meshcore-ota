# Custom firmware

By default the program builds from the official MeshCore repository. It can also build from another git repository or from source folders on the computer, and it can add build flags per device. This is meant for repeaters that need their own changes, for example additional sensors.

Everything described here is part of the firmware. All repeaters of one device type must run identical firmware, so a change to the source, to a local source folder or to the build flags raises the settings revision and reaches the repeaters as an update.

## Another git repository

```
meshcore-ota custom --source owner/name
meshcore-ota custom --source https://example.org/team/firmware.git
meshcore-ota custom --source git@example.org:team/firmware.git
meshcore-ota custom --source default
```

`owner/name` stands for a repository on GitHub. Any other value is passed to git as it is, so every URL and every local path that `git` accepts works. `default` selects the official MeshCore repository again. The same setting is in menu item 9.

Versions are read from the tags of the repository. A tag named `repeater-v1.18.0` or `v1.18.0` becomes version `v1.18.0`. If both exist, `repeater-v1.18.0` is used.

For a source other than the official repository the program calls `git`, which therefore has to be installed.

### Private repositories

The program does not store passwords or tokens. It runs `git` with the access the user has already set up: an SSH key, or a credential helper for HTTPS. Git must reach the repository without asking for a password. Check it with

```
git ls-remote --tags <URL>
```

If this command lists the tags without a prompt, the program can use the repository.

## Source folders

A folder `sources/<version>/` next to the program is listed as a version, for example `sources/v1.18.1/`. The folder contains the complete MeshCore source tree with `platformio.ini` at its top level. The name must be a version in the form `v1.18.1`.

A local folder takes precedence over a tag with the same version. It needs neither git nor an internet connection for the source, only the build tool.

When files in a source folder change, the settings revision rises and the next build gets a new name, for example `v1.18.1+4` instead of `v1.18.1+3`.

## Build flags

```
meshcore-ota custom --device Xiao_nrf52_repeater --define ENV_INCLUDE_BME280=1
meshcore-ota custom --device Xiao_nrf52_repeater --undefine SOME_FEATURE
meshcore-ota custom --device Xiao_nrf52_repeater --clear
```

`--define NAME` or `--define NAME=value` adds a define to the build of this device, `--undefine NAME` removes one that the source sets. Both can be given several times. `--clear` removes all extra flags of the device. In menu item 9 the flags are entered in compiler form, `-DNAME=value` and `-UNAME`. `meshcore-ota custom` without options shows the current source, the local source folders and the flags.

## Requirements for a custom source

- The MeshCore-OTA patch (`firmware/meshcore.patch`) must apply to the source. It touches `examples/simple_repeater/MyMesh.cpp`, `MyMesh.h` and the PlatformIO configuration of some variants. If these files differ too much from MeshCore, the build stops with a message.
- The device must exist as a repeater environment in `platformio.ini` of the source.
- A build runs the build scripts of the source on the computer. Use only sources you trust.

The settings are stored in `custom.json`.
