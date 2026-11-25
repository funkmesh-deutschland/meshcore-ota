# Settings and presets

With `meshcore-ota` you give your repeaters a set of basic settings. They are built into the firmware as the starting values, and every later change reaches the repeaters by radio as a small signed settings package, without a firmware update and without a restart.

![Settings and presets](images/settings.png)

## What a preset is

A preset is a named list of settings, for example `standard` for most repeaters and `dense` for repeaters with many neighbours. Each setting corresponds to a MeshCore `set <name> <value>` command. Anything the respective MeshCore version knows through `set` can be prescribed this way. The menu (menu item 4) suggests these:

| Setting | Meaning |
| --- | --- |
| `tx` | transmit power in dBm |
| `advert.interval` | advert to neighbours, interval in minutes (0 means off) |
| `flood.advert.interval` | advert to the whole mesh, interval in hours (0 means off) |
| `flood.max` | highest number of hops for forwarded flood packets |
| `flood.max.unscoped` | highest number of hops for flood packets without a region |
| `flood.max.advert` | highest number of hops for forwarded adverts |
| `af` | airtime factor (pause after each transmission) |
| `rxdelay` | delay before forwarding, depending on the signal |
| `txdelay` | random delay when forwarding flood packets |
| `direct.txdelay` | random delay when forwarding on a fixed route |
| `multi.acks` | additional acknowledgements (0 or 1) |
| `path.hash.mode` | length of the hops in routes (0 means 1 byte, 1 means 2 bytes, 2 means 3 bytes) |
| `loop.detect` | loop detection (`off`, `minimal`, `moderate`, `strict`) |
| `agc.reset.interval` | reset the receiver gain, interval in seconds (0 means off) |

With `n` any other setting can be entered in the menu under its name from the `set` command. Only settings with a value are set. Everything else stays on the repeater as it is.

Rules:

- At most 8 presets.
- The name of a preset has 1 to 15 characters from a to z, digits, `-` and `_`.
- Values may contain letters, digits and the characters `. , : _ + -`, at most 40 characters.
- Name, location, passwords, owner information and the private key of the repeater cannot be included in a preset.

The last rule has two reasons: all repeaters of a device type carry bit-identical firmware, otherwise no update fits. Whatever differs per device therefore stays in the settings storage of the device. In addition, the contents of the firmware would be readable by anyone over the air.

The presets are stored in `settings.json` in the data directory:

```
{
  "revision": 3,
  "digest": "...",
  "config": 5,
  "config_digest": "...",
  "presets": [
    {"name": "standard", "values": {"tx": "20", "advert.interval": "120"}},
    {"name": "dense", "values": {"tx": "14", "flood.max": "8"}}
  ],
  "builtin": {"config": 4, "presets": [...]}
}
```

`config` is the revision of the settings and rises with every change to a preset. `revision` is the build number of the firmware, the number after the plus sign in `v1.18.0+3`. `builtin` holds the presets that are compiled into the firmware of this build number.

## On the repeater

Every repeater holds all presets, from its firmware or from the latest settings package it has received. Each repeater remembers for itself which one applies. Without a selection of its own, the first preset in the list applies.

| Command | Effect |
| --- | --- |
| `ota preset` | shows the active preset, whether it has been applied, the lock, the number of errors and the list |
| `ota preset <name>` | switches to this preset and applies it immediately |
| `ota locksettings on` | the repeater keeps its values, updates no longer overwrite anything |
| `ota locksettings off` | removes the lock |
| `ota locksettings` | shows the lock |

The commands work over USB and through admin access over the air. The reply to `ota preset` looks like this, for example:

```
OK active=standard state=applied lock=off errors=0 rev=5 list=standard,dense
```

On nRF52840, preset and lock cannot be changed while Bluetooth is active on the repeater (for example after `start ota`). The repeater then replies with `ERR Bluetooth is active, try later`. The same applies to all other `ota` commands that write, see [app.md](app.md).

## When values are overwritten

A repeater applies its preset

- a few seconds after the first start of a firmware with presets,
- when a settings package arrives in which the active preset has different values,
- after a firmware update, if the active preset has different values in it,
- when you switch with `ota preset <name>`.

In between you can change individual values on the repeater by hand. They stay until the active preset arrives with changed values. With `ota locksettings on` they stay even then. A switch with `ota preset <name>` always takes effect, even with the lock set.

If the selected preset no longer exists in new settings, the first one in the list applies again.

If a MeshCore version does not know a setting, the repeater counts that as an error and sets the rest. `ota preset` shows the number of errors.

## How changes are sent out

Every change to a preset raises the revision of the settings. The program signs all presets together as one settings package. It holds at most 1216 bytes, and menu item 4 shows how much is used.

The dashboard distributes the package like this:

- Repeaters in range hear a short note with the revision. Those with an older one ask for the package and receive it in parts of 152 bytes.
- A repeater on a route gets the parts sent directly as soon as its report shows an older revision.
- A repeater that has taken over new settings tells its neighbours, and they fetch the package from it. This way the settings travel from repeater to repeater like an update, also across different device types.
- Every 6 hours a repeater repeats the note, so that repeaters that were switched off catch up.

A repeater takes over a settings package only if it is signed with a key it trusts and its revision is higher than the one in use. It stores the package in the file system of MeshCore, where it survives restarts and firmware updates. The active preset is applied at once, unless the lock is set.

Typical presets fit into one to three radio packets, which is a few seconds of airtime per repeater.

The firmware itself carries the presets as they were when the first firmware of its build number was built. A repeater that is installed fresh starts with those and receives the current ones by radio. Repeaters with firmware from before program version 1.1 do not know settings packages. They get the current presets with the update to a current build.

## The build number

The number after the plus sign in `v1.18.0+4` rises when the key changes, when the firmware source or the build flags change (see [custom.md](custom.md)), and when a new version of the program brings changes to the MeshCore-OTA part of the firmware. The dashboard then builds the running version with the new number and sends the difference as an update. A repeater receives it once it is running the latest released version. If it is one version behind, it receives the next version with the current build number.

## The key

The public key is part of the firmware as well. Details are in [keys.md](keys.md).
