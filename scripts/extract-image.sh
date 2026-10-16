#!/usr/bin/env bash
# Extract the UGen300 runtime image from Hailo's hailort-usb-driver .deb (from ASUS's
# UGen300_USB_FW_Upgrade.zip or Hailo's Developer Zone). No Linux or VM needed.
#   scripts/extract-image.sh hailort-usb-driver_5.3.0_arm64.deb [dest-dir]
set -euo pipefail

readonly IMAGE="core-image-hailo-hailo10-usb-dongle.ext4"
deb="${1:?usage: $0 <hailort-usb-driver_*.deb> [dest-dir]}"
dest="${2:-${HOME}/.local/share/ugen300}"

[[ -f "${deb}" ]] || { echo "no such file: ${deb}" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "${work}"' EXIT

# bsdtar (macOS tar) reads .deb archives directly; elsewhere fall back to ar
if tar --version 2>/dev/null | grep -q bsdtar; then
    data_member=$(tar -tf "${deb}" | grep '^data\.tar')
    tar -xOf "${deb}" "${data_member}" | tar -xf - -C "${work}"
else
    data_member=$(ar t "${deb}" | grep '^data\.tar')
    (cd "${work}" && ar x "${deb}" "${data_member}" && tar -xf "${data_member}")
fi

src="${work}/lib/firmware/hailo/hailo10h_usb/${IMAGE}"
[[ -f "${src}" ]] || { echo "${IMAGE} not found inside ${deb}" >&2; exit 1; }
mkdir -p "${dest}"
cp "${src}" "${dest}/${IMAGE}"
echo "${dest}/${IMAGE}"
shasum -a 256 "${dest}/${IMAGE}" 2>/dev/null || sha256sum "${dest}/${IMAGE}"
