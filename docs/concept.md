# Concept

## Goal

An operator updates their own MeshCore repeaters over LoRa without having to go to the device. The operator alone decides which firmware runs on their devices.

## Trust model

- Each operator has their own Ed25519 key pair. The private key stays on the operator's computer and is never on a repeater.
- The firmware of a repeater carries the public key of its operator inside it. A repeater has two key slots, the second one can be filled over USB.
- A repeater accepts data of an update only once it has the complete manifest and the signature matches one of its keys.
- Without a key a repeater does not react to any offer.
- Keys can be set or removed on the repeater over USB only. Over the air the key changes only through a signed update, see [keys.md](keys.md).

Consequences:

- No key is valid for the whole network. A stolen key only affects the repeaters of its owner.
- A stolen admin password is not sufficient to install firmware. That requires the signing key.
- Whoever loses their key has to go to the device.

## Distribution

### Basic rules

- **Zero-hop only between repeaters.** Update data goes directly from one repeater to a neighbour in radio range. There is no flooding. An update travels from neighbour to neighbour.
- **Only on request.** A repeater sends data only if a neighbour has requested it.
- **0.5 % airtime, built in.** The value applies per repeater to everything MeshCore-OTA sends, including offers, requests and hellos. It cannot be changed on the repeater. At most 10 seconds of airtime are saved up. The test firmware has 5 %.
- **The operator's computer** sends through its companion with 0.5 %, adjustable from 0.05 to 5 %. In addition it reaches repeaters up to 2 hops away on a fixed route.

### Finding neighbours

1. A repeater that has stored blocks of a verified update offers it by zero-hop. The offer states the device type, the size and checksum of the firmware the update starts from, the key ID and which pages it can deliver. Offers are sent at growing intervals, from 1 minute to 1 hour.
2. A neighbour reacts only if everything matches: it trusts the key, has the same device type and is running exactly the source firmware.
3. The neighbour first fetches the signed manifest and verifies the signature. Only then does it accept data.
4. The neighbour tells the provider which pages it is missing. The provider sends them one after another without each having to be requested individually.
5. A repeater already offers what it has while it is still fetching the rest.

Repeaters with a different key and repeaters without MeshCore-OTA do not reply and are not sent anything.

### Overhearing

Data packets carry no recipient. Every repeater that is loading the same update uses every packet it hears, regardless of whom it was sent for and in which order it arrives. Each block on its own yields exactly one page.

- A repeater listens first. Only when nothing usable arrives for a while does it specifically request the pages it is missing.
- If several neighbours send requests, the provider first sends the pages that most of them need.
- Of a large block that arrives in several packets, a repeater only requests the chunks it is missing.

In the simulation (`sim/test_node.c`) a single receiver needs 67 packets from the provider for the test package. Three receivers that join at different times need 105 together instead of 201.

### Switching the provider

If 6 requests remain without usable data, a repeater gives up its provider and asks by zero-hop who else can deliver the update. Every neighbour with matching blocks replies with an offer after a random delay. The repeater continues loading from the one that has the most of the missing pages. Without a reply it asks again at growing intervals.

### The operator's computer

The program `meshcore-ota` controls an ordinary companion over USB or Bluetooth, see [app.md](app.md).

It queries over the air which of the operator's repeaters are present. Each one replies with its device type and the size and checksum of the running firmware. This determines which package is built and offered for whom.

MeshCore does not forward MeshCore-OTA packets by flooding, but it does forward them on a fixed route, also across repeaters without MeshCore-OTA. The computer uses this for repeaters up to 2 hops away. The program learns the route from adverts the companion has heard, from the stored routes of its contacts and from a path discovery to repeaters that the operator has entered as their own. Query, offer and data travel out along this route, hello and requests travel back along the return route. Each packet then costs airtime for every hop.

Repeaters further away receive the update as it travels from neighbour to neighbour.

### Step by step

A package leads from exactly one firmware to exactly one other. The program gives each repeater the next released version after the running one. If a repeater is several versions behind, the steps follow one after another. A repeater keeps up to two older packages and delivers them to stragglers, as far as the storage allows.

### Applying

How an update is applied is specified in the signed manifest and cannot be changed afterwards.

- **On its own** (the default of the program). The repeater applies as soon as it has the complete update and no neighbour has loaded from it for half an hour. On top of that comes a random delay of up to another half hour, so that not all repeaters in an area reboot at the same time.
- **On command** (`--manual-apply`). The repeater waits for `ota apply`, on the device or through admin access in the mesh.

If a device cannot keep the package when applying, it waits 24 hours without a request instead of half an hour before it applies. If a device cannot create a rollback copy, it applies on command only, unless the manifest explicitly allows it (`--no-rollback`). Details per device class are in [devices.md](devices.md).

## Sequence of an update

1. The program builds the new firmware and creates the patch against the firmware that is running on the repeater.
2. It generates the manifest: device type, name of the build, size and SHA-256 of the new firmware, size and SHA-256 of the source firmware and a checksum for each changed page. The manifest is signed.
3. Before the first data packet each repeater checks the key, the device type and the running firmware, then the signature of the manifest. If something does not match, it accepts nothing.
4. The repeater applies every received block and checks the finished page against the manifest. It records the progress in flash, and after a reboot it continues from where it was.
5. When everything is there, it checks the SHA-256 of the new firmware against the manifest.
6. To apply, the repeater writes a job and reboots. The loader (on the ESP32 the firmware itself) verifies and switches.
7. The new firmware runs in a trial boot. It confirms itself only when it demonstrably works. Otherwise the old one runs again after three trial boots.

### Only the changes

What is transferred is a patch that contains only the difference between the running and the new firmware. It is divided into blocks, each of which yields exactly one 4 KB page of the new firmware.

- Pages that do not change compared with the running firmware appear in the manifest as a single bit only. Nothing is transferred for them.
- The manifest carries a table stating which sections of the firmware have shifted and by how much. The repeater uses it to convert branch targets and stored addresses of the old firmware before it applies the difference.
- The blocks are compressed with Deflate. Several small blocks travel together in one radio packet.
- For nRF52840 the program stays with the compiler of the first version built, because a compiler change alters almost every function.
- On the ESP32 an extra script places the libraries at the front of the image and the MeshCore code at the end, so that changes to the MeshCore code do not shift the libraries.

The format is described in [protocol.md](protocol.md).

### Duration

The duration depends on the radio parameters and the number of data packets. The program shows the number of radio packets per package in the dashboard and in `meshcore-ota info`.

Calculated, not measured: at SF8, 62.5 kHz and coding rate 4/5 a full data packet occupies about 1 second of airtime. At 0.5 % a provider may therefore send one roughly every 200 seconds, which is about 18 packets per hour. An update with 50 data packets thus takes just under 3 hours per radio link, one with 150 about 8 hours. Over a route with hops each packet counts for every hop.

A repeater already passes on finished pages while it is still fetching the rest.

## The loader (nRF52840)

The loader is a small program between the SoftDevice and the firmware. It is the most critical part: a bug there renders a repeater unusable. Its logic is in `core/mcota_boot.c` and runs unchanged in the simulation.

### Swap on devices with external flash

Slot A is the running firmware in the internal flash, slot B the new one in the external flash. For every page three steps are performed, and each is recorded in the journal on completion:

1. Copy the page from A to the scratch page.
2. Copy the page from B to A.
3. Copy the scratch page to B.

After a power failure the loader reads the journal and repeats the step that is not recorded as completed. A record only counts if the whole word in flash is zero.

Devices without external flash rebuild the new firmware in place, see [devices.md](devices.md).

### Checks

- Before the switch: SHA-256 of the running firmware and of the new one against the job. If one does not match, the job is rejected and nothing is changed.
- After the switch: SHA-256 of slot A. If it does not match, the loader restores the old firmware.

### Trial boot and rollback

- Before every start of the new firmware the loader records a trial boot.
- The firmware confirms itself. If the confirmation is missing after three trial boots, the loader restores the old firmware on the next start, with a journal of its own.
- Without a valid job the loader writes nothing and starts slot A.

### Write boundaries

In the internal flash the firmware writes only to the status area, to the page with keys and preset selection and, on devices without external flash, to the free area behind the running firmware. The loader writes only from slot A to the end of the status area. The bootloader of the device and the SoftDevice are outside of that. Recovery through the double reset therefore remains available.

As long as the SoftDevice is active (Bluetooth, for example after `start ota`), the firmware does not write to the internal flash. MeshCore-OTA pauses for that time: it ignores its radio packets, refuses commands that write, and `ota info` shows `PAUSED`. Bluetooth stays active until the next restart.

### What the loader cannot do

- It does not verify a signature, only checksums. The signature is verified by the running firmware before it writes the job. Anyone who already executes their own code on the device can therefore give the loader a job. This corresponds to access over USB, which allows any firmware anyway.
- A power failure during a trial boot counts as a trial boot.
- If the new firmware hangs without crashing and without rebooting, the rollback does not take effect.

## Health check

The new firmware confirms itself only when all of the following are met:

- It has been running for at least one minute.
- The running firmware is exactly the one stated in the job.
- The radio works in both directions: a neighbour has answered the repeater. An ordinary MeshCore repeater that replies to neighbour discovery is sufficient for this. A neighbour with MeshCore-OTA is not required.
- No write access to the flash was rejected.

If the confirmation does not happen within 10 minutes, the repeater reboots. After three such trial boots the old firmware runs again, and the update counts as failed.

A repeater without a reachable neighbour therefore does not confirm an update and rolls back.

## Limitations

- **Devices without external flash** may be able to apply a large update only without a rollback copy.
- **One update at a time.** As long as a repeater is loading an update, it does not accept another one. If no usable data arrives for eight waiting periods (about 85 minutes at 0.5 % airtime), it accepts another offer. `ota drop` discards the update at once.
- **Source firmware.** An update can only be built for repeaters whose running firmware is present as a file in the data directory.
- **Jammers in radio range** can stall a transfer but cannot inject anything false.
- **Maintenance.** The project builds on MeshCore. The addition in `firmware/meshcore.patch` has to apply to every new MeshCore version, otherwise the build aborts with a message.

## Relation to other projects

This project is an independent development with its own code and is not affiliated with the MeshCore project. The difference to approaches with a central signing key: here each operator holds their own key.
