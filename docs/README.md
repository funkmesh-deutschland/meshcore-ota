# Documentation

MeshCore-OTA delivers firmware updates to MeshCore repeaters over LoRa. A step-by-step guide to getting started is in the project [README](../README.md).

## For operators

| File | Contents |
| --- | --- |
| [app.md](app.md) | the program `meshcore-ota`: menu, subcommands, options, data directory, commands on the repeater |
| [keys.md](keys.md) | key pair, importing an existing key, key change |
| [settings.md](settings.md) | settings and presets, lock on the repeater |
| [update.md](update.md) | program update, releases, version numbers, CI |

## Technical

| File | Contents |
| --- | --- |
| [concept.md](concept.md) | trust model, distribution, sequence of an update, loader, limitations |
| [devices.md](devices.md) | device classes, memory layout, support status per device |
| [protocol.md](protocol.md) | radio packets, manifest, block format |
| [testing.md](testing.md) | simulation in `sim/`, building by hand, testing on devices |

## Repository layout

| Folder | Contents |
| --- | --- |
| `core/` | core in C: loader logic, manifest, patch, node. Runs on the device and in the simulation |
| `firmware/` | integration with MeshCore (`mcota/`), patch for MeshCore, linker scripts, build script |
| `loader/nrf52/` | loader for nRF52840 |
| `sim/` | simulation and tests |
| `tools/` | the program `meshcore-ota` |
| `.github/workflows/` | tests, building the executables, releases |
