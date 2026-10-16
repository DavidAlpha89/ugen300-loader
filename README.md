# ugen300-loader

Boot the **ASUS UGen300** USB AI accelerator (Hailo-10H) on macOS without a Linux VM.

The UGen300 does not keep its runtime between power cycles. Every time it's plugged in it
comes up as a loader and waits for the host to upload a Linux root filesystem. Hailo ships
that uploader (`hailo_usb_loader`) only as a closed Linux binary. Until now, macOS users had
to boot a VM just to run it.

`ugen300-loader` is a small clean-room reimplementation of that upload over libusb. It's
written from captured USB traffic ([docs/PROTOCOL.md](docs/PROTOCOL.md)), so the stick can go
from plugged in to usable with native macOS tools only.

## Requirements

- Apple Silicon or Intel Mac (it also builds on Linux), `brew install libusb pkg-config`
- A UGen300 that has had ASUS's one-time [first-use firmware update](https://github.com/Asus-UGen-Series/UGen300-USB-Firmware-Upgrade).
  That update writes flash and still needs Linux. `probe` tells you whether your stick has had it.
- The runtime image from Hailo's `hailort-usb-driver_5.3.0_*.deb`, which is inside ASUS's
  `UGen300_USB_FW_Upgrade.zip`. It is not redistributed here.

## Use

```bash
make
scripts/extract-image.sh /path/to/hailort-usb-driver_5.3.0_arm64.deb   # once
./ugen300-loader probe
./ugen300-loader rfs-upload
```

On a MacBook (A18 Pro, macOS 27), with no VM:

```text
$ ./ugen300-loader rfs-upload
Device:   usb bus 0 addr 2  0b05:1d6f  bcdDevice 5.15
Product:  UGen300 USB Loader
Serial:   H10-DEV-001
Mode:     loader (waiting for runtime upload)
Image:    ~/.local/share/ugen300/core-image-hailo-hailo10-usb-dongle.ext4 (103057408 bytes)
Loader:   protocol 1, SKU NA, Hailo Linux build v1.10.10
Uploaded 103057408 bytes in 0.92 s (112 MB/s)
Waiting up to 30 s for the runtime to enumerate...
Device:   usb bus 0 addr 2  0b05:1d6f  bcdDevice 1.00
Product:  UGen300 USB AI Accelerator (Hailo-10H)
Serial:   HRT-USB-001
Mode:     runtime (HailoRT can use it)
Runtime is up.

$ hailortcli fw-control identify      # relaxcloud-cn/hailort-macos-usb build
Executing on device: usb/000:002
Identifying board
Control Protocol Version: 2
Firmware Version: 5.3.0 (release,app)
Logger Version: 0
Device Architecture: HAILO10H
```

Once the runtime is up, a macOS HailoRT build talks to the stick directly, for example
[relaxcloud-cn/hailort-macos-usb](https://github.com/relaxcloud-cn/hailort-macos-usb).

## Scope

- `probe` only reads; `rfs-upload` only writes RAM on the stick.
- `fw-update` is intentionally not implemented, because it writes the stick's flash. Use
  ASUS's procedure on Linux.
- The loader refuses a stick that still has its factory loader, and checks the loader's build
  string (`--expect-build` / `--force` override it).

## License

MIT, for this code only. HailoRT, firmware and the runtime image belong to Hailo/ASUS and
stay under their terms.
