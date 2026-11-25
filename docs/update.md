# Program update, releases and CI

## Updating the program

On start the menu checks GitHub in the background for the latest release of `funkmesh-deutschland/meshcore-ota`. The check has a timeout of 6 seconds and does not hold up the menu. If there is a newer version, a notice appears above the menu.

Menu item 8 shows the new version with its changes and asks whether to update. The same without the menu:

```
meshcore-ota self-update
```

Sequence:

1. The program downloads the file for its own operating system (`meshcore-ota-linux`, `meshcore-ota-linux-arm64` or `meshcore-ota-windows.exe`) next to the running executable.
2. It renames the running file to `<name>.old` and puts the new one in its place. If that fails, it restores the old one.
3. On Linux it deletes the old file immediately, on Windows on the next start.
4. Then restart the program.

Only the executable is replaced. Keys, firmware, packages and settings in the data directory remain unchanged.

```
meshcore-ota version
```

shows the version of the program.

The self-update is only available for the prebuilt executable. When running from source, update with git. The version from source is `0.0.0`, so the check reports every release as newer there.

If GitHub cannot be reached, the program reports that no newer version was found and carries on.

## Version numbers

The version has three parts, for example `v1.0.7`. The first two are in the file `VERSION` in the repository. The third is incremented by the workflow on every release. When `VERSION` changes, the third part starts again at 0.

During the build the workflow writes the version to `tools/mcota_version.py`.

## Releases

Every release on GitHub contains:

| File | Contents |
| --- | --- |
| `meshcore-ota-linux` | program for Linux on x86-64 |
| `meshcore-ota-linux-arm64` | program for Linux on ARM64 |
| `meshcore-ota-windows.exe` | program for Windows |
| `CHANGELOG.md` | changes of all versions |

The release notes list the commits since the previous release.

## Branches

Work happens on the branch `develop`. Changes reach it through pull requests from feature branches. `main` only receives a pull request from `develop` when a version is ready, and that merge creates the release. Several changes therefore end up in one version.

## CI

The workflow `.github/workflows/release.yml` runs on every push to `main`, on every pull request and on manual dispatch.

| Job | What it does |
| --- | --- |
| `test` | checks out MeshCore at a pinned commit, installs `tools/requirements.txt` and runs `make -C sim test` |
| `version` | determines the next version number from `VERSION` and the latest tag |
| `build` | builds the executables for Linux, Linux ARM64 and Windows with `python tools/build_app.py` |
| `firmware` | builds a repeater firmware with each finished executable on its own system |
| `release` | only on a push to `main` and only if all other jobs succeeded: creates the release with tag, executables and changelog |

Every merge to `main` therefore results in a release.

## Building the executable yourself

```
pip install -r tools/requirements.txt pyinstaller
python tools/build_app.py
```

The result is in `dist/meshcore-ota` (on Windows `dist/meshcore-ota.exe`). Besides the program, the executable contains the sources for the firmware build: `core/`, `firmware/mcota/`, `firmware/boards/`, `firmware/esp32/`, `firmware/meshcore.patch` and `loader/nrf52/`, and the page of the web interface from `tools/web/`.
