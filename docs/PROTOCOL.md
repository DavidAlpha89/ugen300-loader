# UGen300 loader protocol

How the ASUS UGen300 (Hailo-10H, USB `0b05:1d6f`) gets its runtime, worked out from usbmon
captures of Hailo's official `hailo_usb_loader` / `hailo_rfs_upload` (HailoRT 5.2.2 and 5.3.0)
on Linux. No vendor binary was disassembled; everything here is what crossed the wire.

## Device states

The stick runs a small Linux (kernel 5.15) from its own flash and boots it into one of these
USB identities:

| Product string | Serial | bcdDevice | Meaning |
|---|---|---|---|
| `ASUSTek Computer, Inc. Hailo Composite USB Gadget` | `H10-DEV-001` | 5.15 | Factory loader. Needs ASUS's mandatory first-use update (Linux only). |
| `UGen300 USB Loader` | `H10-DEV-001` | 5.15 | Updated loader. Waits for the host to upload the runtime rootfs. |
| `UGen300 USB AI Accelerator (Hailo-10H)` | `HRT-USB-001` | 1.00 | Runtime: HailoRT's FunctionFS gadget with 5 vendor interfaces. |

The runtime lives in RAM, so the stick falls back to the loader on every power cycle and the
rootfs has to be uploaded again.

Both loader identities expose two configurations, each with interface 0 (class `ff/00/01`), bulk
OUT `0x01` and interrupt IN `0x81`:

| Configuration | Name | Used for |
|---|---|---|
| 1 | `Hailo RFS Loading Mode` | uploading a rootfs image |
| 2 | `Hailo Software Update Mode` | sending a `.swu` firmware update (after booting the `-swu.ext4` rootfs) |

Linux selects configuration 1 when the device appears. **macOS leaves it unconfigured**, so a
macOS host must call `SET_CONFIGURATION 1` before it claims interface 0.

## Vendor requests

All requests go to the device (recipient = device), with wIndex 0 unless noted. IN requests
use bmRequestType `0xC0`; OUT requests use `0x40`.

| bRequest | Dir | wValue | wLength | Meaning |
|---|---|---|---|---|
| `0x16` | IN | 0 | 4 | Loader protocol version, u32 LE. Seen: `1`. |
| `0x14` | IN | 0 | 64 | SKU ID, NUL-terminated. Seen: `NA`. |
| `0x11` | IN | 0 | 64 | Build string, NUL-padded. Factory: `Hailo Linux build v1.9.0`; after the 5.3.0 update: `Hailo Linux build v1.10.10`. |
| `0x12` | OUT | size[15:0] | 0 | Upload start. **wIndex = size[31:16]**, so the image size travels as wIndex:wValue. |
| `0x15` | IN | 0 | 1 | Receiver ready (`1` once up). |
| `0x15` | IN | 1 | 4 | RX byte counter, u32 LE. |
| `0x15` | IN | 2 | 1 | Ready for image data (`1`). |
| `0x15` | IN | 3 | 8 | Firmware-update state (u32 state, then an s32 result): `1, -4096` while applying, `2, 0` when done OK. |
| `0x16` | OUT | 0 | 0 | Reboot the loader (sent after a firmware update completes). |

The vendor tools check the protocol version (`1`) and the build string before an `rfs-upload`.
Each tool expects exactly the build its HailoRT release shipped: 5.2.2 expects `v1.10.0`,
5.3.0 expects `v1.10.10`.

## rfs-upload sequence

The timeout is 10 s per transfer and polls run every 100 ms.

1. `SET_CONFIGURATION 1`, then claim interface 0.
2. Read `0x16` (version), `0x14` (SKU) and `0x11` (build), and validate them.
3. Release interface 0, `SET_CONFIGURATION 1` again, then re-claim.
4. `0x12` OUT with the image size. For the 5.3.0 runtime, wValue=`0x8800` and wIndex=`0x0624`
   (103,057,408 bytes).
5. Poll `0x15`/0 until it reads `1`. It reads `0` first and `1` about 100 ms later.
6. Bulk OUT 64 × `0xAA` ("ping"), then read `0x15`/1. This first read returns `0` and clears the counter.
7. Wait 100 ms, send a second ping, and read `0x15`/1, which should return `64`. A mismatch is only a warning in the vendor tool.
8. Read `0x15`/2 and expect `1`.
9. Bulk OUT the image in 65,536-byte transfers (about 75 MB/s on USB 3 Gen 1), then send **one
   zero-length bulk transfer** to mark the end.
10. The loader boots the runtime and disconnects about 4 s later. The runtime gadget enumerates
    about 5 s after the zero-length transfer.

The image is `core-image-hailo-hailo10-usb-dongle.ext4` from `hailort-usb-driver_<ver>_<arch>.deb`
(`/lib/firmware/hailo/hailo10h_usb/`). The arm64 and amd64 packages carry byte-identical images.

## fw-update sequence (for reference only; not implemented)

1. `libusb_reset_device`, then wait about 3 s.
2. Read version and SKU.
3. Run steps 3–9 of the rfs-upload sequence on configuration 1 with `core-image-…-swu.ext4`, the
   60,547,072-byte update rootfs.
4. `SET_CONFIGURATION 2`, then repeat the start/ping/ready handshake and bulk the `.swu`. The
   `.swu` is a cpio "newc" archive with magic `070702`.
5. Poll `0x15`/3 until the state reads `2`. Stage 2 took about 90–106 s.
6. `0x16` OUT reboots the loader. The 5.3.0 tool then re-scans and performs an rfs-upload itself.

ASUS's first-use procedure runs this twice. The first pass uses the 5.2.2 tool with the
"provisionV3" `.swu`: the update applies (state `2, 0`), but the tool then reports failure
because the loader still says `v1.9.0`. The second pass uses the 5.3.0 tool, which installs the
`v1.10.10` loader and renames it to `UGen300 USB Loader`.
