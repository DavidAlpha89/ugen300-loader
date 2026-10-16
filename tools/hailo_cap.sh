#!/usr/bin/env bash
# USB capture helper for the UGen300 (0b05:1d6f) on Linux.
#   hailo_cap.sh start <name>   start a usbmon pcap on the stick's bus
#   hailo_cap.sh stop           stop it and print the capture size
#   hailo_cap.sh state          show the stick's current USB identity
set -euo pipefail

CAP_DIR="${HOME}/hailo-caps"
PID_FILE="${CAP_DIR}/.tcpdump.pid"
VIDPID="0b05:1d6f"

bus_of_stick() {
    local line
    line=$(lsusb -d "${VIDPID}" | head -n1)
    [[ -n "${line}" ]] || { echo "stick ${VIDPID} not found" >&2; exit 1; }
    echo $((10#$(awk '{print $2}' <<<"${line}")))
}

case "${1:-}" in
    start)
        name="${2:?capture name}"
        mkdir -p "${CAP_DIR}"
        sudo modprobe usbmon
        bus=$(bus_of_stick)
        # -B: kernel ring in KiB (bulk payloads are large); -s 0: whole packets
        sudo nohup tcpdump -i "usbmon${bus}" -B 65536 -s 0 -U -w "${CAP_DIR}/${name}.pcap" \
            >"${CAP_DIR}/${name}.tcpdump.log" 2>&1 &
        echo $! >"${PID_FILE}"
        sleep 1
        echo "capturing usbmon${bus} -> ${CAP_DIR}/${name}.pcap (pid $(cat "${PID_FILE}"))"
        ;;
    stop)
        [[ -f "${PID_FILE}" ]] || { echo "no capture running"; exit 0; }
        sudo pkill -INT -f "tcpdump -i usbmon" || true
        rm -f "${PID_FILE}"
        sleep 1
        ls -lh "${CAP_DIR}"/*.pcap
        ;;
    state)
        lsusb -v -d "${VIDPID}" 2>/dev/null | grep -E 'iProduct|iSerial|bcdDevice|iConfiguration|bDeviceClass' || echo "stick not present"
        ;;
    *)
        echo "usage: $0 {start <name>|stop|state}"; exit 1 ;;
esac
