#!/bin/bash
# Setup loop device for block device testing
# Usage: sudo ./setup_loop_device.sh [size_mb]

set -e

SIZE_MB=${1:-512}  # Default 512MB
IMAGE_FILE="/tmp/xTdb_test_device.img"
LOOP_DEVICE=""

echo "=== xTdb Block Device Test Setup ==="
echo "Creating ${SIZE_MB}MB test image..."

# Create sparse file
dd if=/dev/zero of="$IMAGE_FILE" bs=1M count="$SIZE_MB" 2>/dev/null

echo "Setting up loop device..."
LOOP_DEVICE=$(losetup -f --show "$IMAGE_FILE")

if [ -z "$LOOP_DEVICE" ]; then
    echo "ERROR: Failed to create loop device"
    rm -f "$IMAGE_FILE"
    exit 1
fi

echo "Loop device created: $LOOP_DEVICE"
echo ""
echo "=== Test Device Ready ==="
echo "Device: $LOOP_DEVICE"
echo "Size: ${SIZE_MB}MB"
echo "Image: $IMAGE_FILE"
echo ""
echo "Run tests with:"
echo "  XTDB_TEST_DEVICE=$LOOP_DEVICE ./test_block_device_advanced"
echo ""
echo "To cleanup after testing:"
echo "  sudo losetup -d $LOOP_DEVICE"
echo "  rm $IMAGE_FILE"
