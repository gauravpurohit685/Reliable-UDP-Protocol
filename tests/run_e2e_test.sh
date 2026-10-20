#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$DIR/.." && pwd)"
BIN_DIR="$ROOT_DIR/bin"

PROXY="$BIN_DIR/loss_proxy"
RECEIVER="$BIN_DIR/rudp_receiver"
SENDER="$BIN_DIR/rudp_sender"
TEST_IN="test_input_5mb.bin"
TEST_OUT="received_output_5mb.bin"

echo "===================================================================="
echo "  QUALCOMM RUDP PROTOCOL: END-TO-END RESILIENCE TEST (BASH)         "
echo "===================================================================="

# Generate 1MB random file
dd if=/dev/urandom of="$TEST_IN" bs=1048576 count=1 status=none
SRC_HASH=$(sha256sum "$TEST_IN" | awk '{print $1}')
echo "[*] Source File SHA-256: $SRC_HASH"

# Start Loss Proxy in background (15% drop, 5% duplicate, 20-50ms jitter)
"$PROXY" -l 8000 -t 127.0.0.1 -p 9001 -d 0.15 -u 0.05 -m 20 -M 50 > proxy.log 2>&1 &
PROXY_PID=$!
sleep 0.5

# Start Receiver in background
"$RECEIVER" -p 9001 -o "$TEST_OUT" > receiver.log 2>&1 &
RECV_PID=$!
sleep 0.5

# Run Sender targeting Loss Proxy
"$SENDER" -f "$TEST_IN" -s 127.0.0.1 -p 8000
wait $RECV_PID || true
kill -9 $PROXY_PID 2>/dev/null || true

# Verify Hash
DST_HASH=$(sha256sum "$TEST_OUT" | awk '{print $1}')
echo "===================================================================="
echo " Source SHA-256:   $SRC_HASH"
echo " Received SHA-256: $DST_HASH"
echo "===================================================================="

if [ "$SRC_HASH" == "$DST_HASH" ]; then
    echo "[SUCCESS] 100% BIT-EXACT MATCH UNDER 15% LOSS & 50MS JITTER!"
    rm -f "$TEST_IN" "$TEST_OUT" proxy.log receiver.log
    exit 0
else
    echo "[ERROR] Hash mismatch!"
    exit 1
fi
