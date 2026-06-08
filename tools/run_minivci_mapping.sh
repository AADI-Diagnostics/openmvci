#!/usr/bin/env zsh
set -euo pipefail
set +x

NODE="${1:-/dev/cu.usbserial-A6RUMHAY}"
PCAP="${2:-${HOME}/Downloads/usb-msvc32.pcapng}"
BUILD_DIR="${3:-$(cd "$(dirname "$0")/.. && pwd)/build}"
ICVM_ATTEMPTS="${MAPPING_ICVM_ATTEMPTS:-5}"

wait_for_node() {
  local requested="$1"
  local tries=25
  local sleep_s=0.2
  local i
  for ((i=1; i<=tries; ++i)); do
    if [[ -e "$requested" ]]; then
      echo "$requested"
      return 0
    fi

    local candidates
    candidates=$(find /dev -maxdepth 1 -type c \( \
      -name 'cu.usbserial*' -o \
      -name 'cu.usbmodem*' -o \
      -name 'cu.wch*' -o \
      -name 'cu.SLAB*' \
    \) 2>/dev/null | sort || true)
    if [[ -n "$candidates" ]]; then
      if [[ "$requested" == *usbserial* ]]; then
        local serial_pick
        serial_pick=$(echo "$candidates" | grep 'usbserial' | head -n1 || true)
        if [[ -n "$serial_pick" ]]; then
          echo "$serial_pick"
          return 0
        fi
      fi
      echo "$candidates" | head -n1
      return 0
    fi

    sleep "$sleep_s"
  done
  return 1
}

if [[ ! -x "$BUILD_DIR/mvci_probe" ]]; then
  echo "error: mvci_probe not found at $BUILD_DIR/mvci_probe"
  echo "hint: run 'cmake --build $BUILD_DIR -j4 --target mvci_probe'"
  exit 1
fi

if ! resolved_node=$(wait_for_node "$NODE" | grep '^/dev/cu\.' | tail -n1); then
  echo "error: serial node not present after retries: $NODE"
  echo "hint: replug/power-cycle adapter, then rerun"
  exit 2
fi

if [[ "$resolved_node" != "$NODE" ]]; then
  echo "info: using re-enumerated node: $resolved_node"
  NODE="$resolved_node"
fi

if [[ ! -f "$PCAP" ]]; then
  echo "error: pcap not found: $PCAP"
  exit 3
fi

cd "$BUILD_DIR"

echo "[1/3] Baseline startup replay (first 20 steps, with rx dump)"
./mvci_probe "$NODE" \
  --ctrl-mode none \
  --baud 115200 \
  --pcap "$PCAP" \
  --pcap-start-offset 0 \
  --pcap-max-steps 20 \
  --pcap-cycles 1 \
  --pcap-gap-scale 1 \
  --pcap-show-rx

echo "[2/3] ICVM direct probes after replay"
icvm_got_rx=0
for ((attempt=1; attempt<=ICVM_ATTEMPTS; ++attempt)); do
  echo "  attempt $attempt/$ICVM_ATTEMPTS"
  icvm_out=$(./mvci_probe "$NODE" \
    --ctrl-mode none \
    --baud 115200 \
    --pcap "$PCAP" \
    --pcap-start-offset 0 \
    --pcap-max-steps 20 \
    --pcap-cycles 1 \
    --pcap-gap-scale 1 \
    --send-hex 4943564d010000000500000000000000000000000300000022f190 \
    --send-hex 4943564d01000000050000000000000000000000020000000902 \
    --send-hex 4943564d01000000050000000000000000000000030000001902ff \
    --send-read-ms 500)
  echo "$icvm_out"

  if echo "$icvm_out" | grep -q "custom 01 tx.*rx\\["; then
    icvm_got_rx=1
    break
  fi
done

if [[ "$icvm_got_rx" -eq 0 ]]; then
  echo "  note: no ICVM custom replies after $ICVM_ATTEMPTS attempts"
fi

echo "[3/3] Raw UDS direct probes after replay"
./mvci_probe "$NODE" \
  --ctrl-mode none \
  --baud 115200 \
  --pcap "$PCAP" \
  --pcap-start-offset 0 \
  --pcap-max-steps 20 \
  --pcap-cycles 1 \
  --pcap-gap-scale 1 \
  --send-hex 22f190 \
  --send-hex 0902 \
  --send-hex 1902ff \
  --send-read-ms 500

echo "done"
