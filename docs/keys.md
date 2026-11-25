# Keys

Each operator signs their updates with their own Ed25519 key. The repeaters only know the public part and accept an update only if the manifest is signed with it.

## Files

| File | Contents |
| --- | --- |
| `keys/signing.key` | private key, 32 bytes as 64 hex characters. Secret |
| `keys/public.txt` | public key, key ID and the command `ota key set 1 ...` for firmware built elsewhere |
| `keys/retired-<id>.key` | retired private key after a key change |
| `keys/retired.json` | list of the retired keys with ID and public key |
| `firmware/keys.json` | ID of the key each firmware build was built with |

The key ID is the first 8 bytes of the SHA-256 of the public key, as 16 hex characters. It appears in the menu, in every manifest and in the radio packets.

Whoever has `signing.key` can install firmware on your repeaters. Keep a copy in a separate place. On Linux the program creates the file readable by the owner only.

## Generating

On the first start of the menu and on `meshcore-ota init` a new key pair is created if none exists yet. An existing key is not overwritten.

Menu item 7 shows the ID of the active key, the path of the file and retired keys.

## The key in the firmware

Firmware built by `meshcore-ota` carries the public key inside it. Nothing has to be entered on the repeater for this.

A repeater has two key slots. The built-in key occupies the first free one. With `ota key set <1 or 2> <64 hex characters>` a further public key can be stored over USB, with `ota key clear <1 or 2>` it can be removed. Both work only over USB, never over the air. `ota key` shows the IDs of both slots.

Operators who trust each other can store a shared key as the second one this way. Their repeaters then pass updates signed with it on to each other.

## Importing an existing key

```
meshcore-ota key-import <file>
```

or menu item 7 with the path of the file. The file contains the private key as 32 bytes, raw or as 64 hex characters. The program does not accept other formats.

If the key is already the active one, nothing changes. If no firmware built with the previous key exists yet, the new key replaces it. In that case the previous key is not kept. Otherwise a key change begins.

## Key change

A change is necessary when a different key is to apply, for example when merging two data directories or when the previous key is no longer considered secret.

What happens on import:

1. The previous private key is copied to `keys/retired-<id>.key` and entered in `keys/retired.json`.
2. The new key becomes `keys/signing.key`, and `keys/public.txt` is rewritten.
3. The build number rises, `v1.18.0+3` becomes `v1.18.0+4`.

What happens over the air afterwards:

1. The dashboard queries for repeaters with the new key and with the retired keys.
2. A repeater that still works with the previous key receives an update that is signed with the previous key. Which key belongs to which build is recorded in `firmware/keys.json`.
3. The new firmware in this update carries the new key as the built-in one and knows the earlier ones (up to four) as retired.
4. After the update only the new key is valid on the repeater. A retired key is no longer accepted, even if it was stored by hand.

Things to note:

- Keep `keys/retired-<id>.key` until all repeaters have been switched over. If the file is missing, no update can be signed any more for repeaters with the previous key.
- Repeaters that cannot be reached over the air need the new firmware once over Bluetooth or USB (menu item 3).
- A repeater passes on the update containing the key change only as long as it has not applied it itself. After that it no longer accepts the signature made with the previous key and discards the stored package. Neighbours that have not loaded it by then receive it from the computer.
- The change does not protect retroactively. As long as a repeater still carries the old firmware, it accepts updates signed with the previous key.

## Loss of the key

Without the private key no update can be signed any more. The repeaters then need new firmware with a new key once, over Bluetooth or USB. A second key stored by hand and kept separately avoids this.

## What the key does not protect

- Access over USB allows any firmware, regardless of the key.
- The admin password of a repeater is not sufficient to install firmware over the air. It does, however, allow the commands `ota apply`, `ota drop`, `ota preset` and `ota locksettings`.
- The computer does not need a key to send a finished package (`meshcore-ota send`). Verification takes place on every repeater.
