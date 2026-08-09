#!/usr/bin/env bash
set -euo pipefail

# Publish a freshly built ESP32 firmware binary into the TrueNAS OTA repository.
#
# Usage:
#   ./publish-firmware.sh 0.1.0 /path/to/firmware.bin
#
# The Universal Remote container should map:
#   /mnt/App/UniversalRemote/Firmware -> /data/firmware

VERSION="${1:-}"
SOURCE="${2:-}"

TARGET_DIR="/mnt/App/UniversalRemote/Firmware"
TARGET_FILE="${TARGET_DIR}/universal-remote.bin"
VERSION_FILE="${TARGET_DIR}/version.txt"

if [[ -z "$VERSION" || -z "$SOURCE" ]]; then
    echo "Usage: $0 VERSION /path/to/firmware.bin" >&2
    exit 2
fi

if [[ ! -f "$SOURCE" ]]; then
    echo "Firmware file not found: $SOURCE" >&2
    exit 2
fi

mkdir -p "$TARGET_DIR"

TMP="${TARGET_FILE}.tmp"

cp "$SOURCE" "$TMP"
sync "$TMP"

mv "$TMP" "$TARGET_FILE"
printf '%s\n' "$VERSION" > "$VERSION_FILE"

echo "Published firmware ${VERSION}"
echo "Binary: ${TARGET_FILE}"
echo "SHA256: $(sha256sum "$TARGET_FILE" | awk '{print $1}')"
