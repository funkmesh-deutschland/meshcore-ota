# Radio protocol and package format

All packets are MeshCore raw data packets (`PAYLOAD_TYPE_RAW_CUSTOM`) with direct routing (`ROUTE_TYPE_DIRECT`). Without a route they only reach neighbours in radio range and are not forwarded. Repeaters always send this way. The operator's computer can give a packet a fixed route of up to 2 hops. MeshCore then forwards it across the listed repeaters, and the repeater at the end of the route processes it.

Numbers are stored least significant byte first. The constants are in `core/mcota_node.h` and `core/mcota_manifest.h`.

## Header

| Byte | Contents |
| --- | --- |
| 0, 1 | `L2` |
| 2 | packet type |
| 3 | counter, incremented by one with every packet, random start value |

The counter ensures that two packets never look the same. Otherwise MeshCore discards the second one as a repeat.

Including the header, a packet is at most 176 bytes long.

## Offer (type 1)

A node with stored blocks announces what it can deliver.

| Length | Contents |
| --- | --- |
| 4 | ID of the sender, the first 4 bytes of its public key |
| 8 | ID of the update, the first 8 bytes of the SHA-256 of the manifest |
| 8 | ID of the signing key |
| 4 | checksum of the device type, the first 4 bytes of the SHA-256 of its name |
| 4 | size of the source firmware |
| 8 | beginning of the SHA-256 of the source firmware |
| 2 | number of pages |
| 2 | length of the manifest |
| 1 | flags: bit 0 means the offer is addressed to a single repeater on a fixed route |
| pages / 8, rounded up | list of the deliverable pages, one bit per page |
| 4 | only with bit 0: ID of the target |
| 1 | only with bit 0: length of a hop in bytes, 1 to 3 |
| 1 | only with bit 0: number of hops |
| hops times length | only with bit 0: return route from the target to the sender, at most 6 bytes |

With bit 0 only the target replies, and it does so along the return route. Offers with bit 0 are only sent by the operator's computer. Repeaters offer without a route.

## Request (type 2)

| Length | Contents |
| --- | --- |
| 4 | ID of the provider |
| 4 | ID of the requester |
| 8 | ID of the update |
| 1 | 0 for the manifest, 1 for pages |

For the manifest a mask of 4 bytes follows: one bit per missing chunk.

For pages the following fields follow:

| Length | Contents |
| --- | --- |
| pages / 8, rounded up | list of the missing pages |
| 1 | number of entries that follow, at most 6 |
| 6 each | page (2 bytes) and mask of the missing chunks (4 bytes) for a partially received block |

The provider remembers the missing pages per requester and sends them without further requests. Pages needed by several requesters come first. A repeater remembers up to 4 requesters.

## Data (type 3)

| Length | Contents |
| --- | --- |
| 4 | the first 4 bytes of the ID of the update |
| 1 | 0 chunk of the manifest, 1 chunk of a block, 2 bundle of whole blocks |
| 2 | page, for a bundle the first one |
| 1 | number of the chunk, for a bundle the number of blocks |
| up to 152 | payload |
| 2 | CRC-16 (CCITT, start value `0xFFFF`) over everything from the ID onwards |

A chunk has 152 bytes (`MCOTA_CHUNK`), the last one of a block fewer. The length is divisible by 4 so that every chunk falls on a word boundary in flash. With 152 bytes a data packet including a route over 2 hops of 3 bytes each stays within the 176 bytes. A bundle contains several consecutive small blocks that together have at most 152 bytes. Their lengths are in the manifest.

Data packets carry neither sender nor recipient. Every node with the same update uses what it hears, in any order.

## Ping and pong (types 4 and 5)

Ping: ID of the sender (4), random number (4). Pong: ID of the pinging node (4), ID of the replying node (4), the same random number (4).

A repeater in a trial boot pings to check whether its radio works in both directions.

## Seek (type 6)

ID of the seeking node (4), ID of the update (8). Whoever can deliver the update replies with an offer after a random delay.

## Query and hello (types 7 and 8)

With the query the operator's computer looks for repeaters: ID of the querying node (4), ID of the signing key (8).

For a repeater that is not in direct range these are followed by the ID of the target (4), the length of a hop (1), the number of hops (1) and the return route from the target to the computer. The query then travels on a fixed route across the repeaters in between. Only the target replies, after 0.5 to 2.5 seconds and along the return route.

A repeater that trusts this key replies with a hello, to a query without a route after a random delay of up to 30 seconds:

| Length | Contents |
| --- | --- |
| 4 | ID of the repeater |
| 8 | ID of the signing key |
| 4 | checksum of the device type |
| 4 | size of the running firmware |
| 8 | beginning of the SHA-256 of the running firmware |
| up to 15 | name of the build as text |
| 1 | zero byte, ends the name |
| 1 | state of the update: 0 idle, 1 loading, 2 ready, 3 applied, 4 failed |
| 1 | state of the loader: 0 idle, 1 swap, 2 trial, 3 confirmed, 4 rollback, 5 rejected |
| 4 | beginning of the SHA-256 of the firmware the last job installed, zero without a job |

From this the computer recognises which firmware is running and which package fits. It does not offer an update again that the repeater has rolled back or rejected, and it shows a running trial boot.

## Sequence at the receiver

1. Offer heard, and key, device type and running firmware match: request the manifest.
2. Manifest complete: verify the signature. Only after that is data accepted.
3. Overhear. Every block heard is applied and checked against the manifest.
4. If nothing usable arrives for a while, request the missing pages. The waiting time is twice the interval at which a provider may send, plus 5 seconds and a random component.
5. If 6 requests remain without usable data, give up the provider and look for another one with a seek.

## Airtime

Each repeater charges the airtime of all its own MeshCore-OTA packets against a credit that grows with 0.5 % of the elapsed time and is at most 10 seconds. If the credit is not sufficient, the packet waits. The operator's computer keeps the same credit for its companion with the configured share (0.05 to 5 %) and charges a packet over a route additionally for every hop.

## Manifest

| Length | Contents |
| --- | --- |
| 4 | `LOTA` |
| 1 | format, currently 5 |
| 1 | flags, see below |
| 2 | number of pages |
| 4 | size of the new firmware |
| 4 | size of the source firmware |
| 4 | build number |
| 16 | device type |
| 16 | name of the build |
| 32 | SHA-256 of the new firmware |
| 32 | SHA-256 of the source firmware |
| 8 | ID of the signing key |
| pages / 8, rounded up | one bit per page: the page is identical to the page at the same position in the source firmware |
| 10 each | for every other page: length of the block (2) and the first 8 bytes of the SHA-256 of the finished page |
| 1 to 3 | reference address divided by 4,096, as a variable-length integer. On the nRF52840 the start address of the firmware in flash, on the ESP32 the distance between address and position in the image for the program code |
| 1 | number of entries in the shift table |
| per entry | distance to the end of the previous section, length of the section, shift, each as a variable-length integer |
| 4 numbers | only with flag bit 5: start and length of the program code in the old image, then in the new one, as variable-length integers |
| 64 | Ed25519 signature over everything before it |

Flags:

| Bit | Meaning |
| --- | --- |
| 0 | repeaters apply on their own |
| 1 | the blocks can be applied in place |
| 2 | when doing so, from the last page to the first |
| 3 | devices without room for a rollback copy apply on their own anyway. Without this flag they wait for `ota apply` |
| 4 | branch targets (ARM Thumb) are converted |
| 5 | load instructions for constants (Xtensa, ESP32) are converted |

Nothing is transferred for identical pages. A manifest has at most 4,864 bytes (32 chunks of 152 bytes), the shift table at most 1,200 bytes.

The ID of the update is the first 8 bytes of the SHA-256 over the whole manifest including the signature.

### Variable-length integers

Seven bits of the number per byte, lowest first. The top bit indicates whether another byte follows. Signed numbers are converted beforehand: 0, -1, 1, -2, 2 becomes 0, 1, 2, 3, 4.

### Shift table

Each entry describes a section of addresses and by how much it has shifted in the new firmware. The sections are sorted in ascending order and count from the reference address. Sections in RAM are in the same table.

Before the repeater applies a block, it converts the page read from the source firmware:

1. Every 32-bit word at a position divisible by four whose value (without the lowest bit) lies in a section of the table is increased by the shift.
2. With flag bit 4: every branch instruction (BL and B.W) is converted from an offset into its target. If the target lies in a section of the table, the shift is added.
3. With flag bit 5: in the program code every load instruction for a constant (L32R) is converted from an offset into the position of the constant, likewise with the shift.

The finished page carries targets instead of offsets and is converted back at the end.

### Block

A block is compressed with Deflate (raw, without zlib header) and describes exactly one page. Compressed it has at most 4,800 bytes.

| Length | Contents |
| --- | --- |
| 1 | number of segments (bits 0 to 6, at most 80). Bit 7: the differences are in the sparse form |
| per segment | length of the segment from the source firmware, length of the new data after it, and if the segment is not empty, its position in the source firmware as a deviation from the expected position. All as variable-length integers |
| | the differences to the segments from the source firmware |
| | the new data of all segments |

The expected position is initially the position of the page itself and afterwards the end of the previous segment plus the length of the new data in between.

The differences are added byte by byte to the source firmware. In the simple form there is one value per byte. In the sparse form there is first the number of non-zero values, then for each the number of zeros before it, then the values themselves. The program takes the smallest form per page, and if in doubt the whole page as new data.

## Package file

A `.mcota` file contains the length of the manifest (2 bytes), the manifest and then the blocks of all changed pages in ascending order. The lengths of the blocks are in the manifest. `meshcore-ota info <package>` shows the contents.
