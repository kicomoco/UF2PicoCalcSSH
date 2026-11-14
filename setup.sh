#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$ROOT/.deps"
cd "$ROOT/.deps"
fetch() {
    local name="$1" url="$2" commit="$3"
    if [ ! -d "$name" ]; then
        git init -q "$name"
        git -C "$name" remote add origin "$url"
        git -C "$name" fetch --depth 1 origin "$commit"
        git -C "$name" checkout --detach FETCH_HEAD
    fi
    if [ "$(git -C "$name" rev-parse HEAD)" != "$commit" ]; then
        echo "$name is not at the expected revision; move it aside and rerun setup." >&2
        exit 1
    fi
}
fetch wolfssh https://github.com/wolfSSL/wolfssh.git 8221e4ee48c93da6cea3325a91de2ecc00ae5f56
fetch wolfssl https://github.com/wolfSSL/wolfssl.git 3c5eead44904df64e6a5a1f4ebdce377d35a849a
if [ -z "${PICO_SDK_PATH:-}" ]; then
    fetch pico-sdk https://github.com/raspberrypi/pico-sdk.git a1438dff1d38bd9c65dbd693f0e5db4b9ae91779
    git -C pico-sdk submodule update --init --depth 1 lib/cyw43-driver lib/lwip lib/tinyusb
fi
if ! command -v arm-none-eabi-gcc >/dev/null && [ -z "${PICO_TOOLCHAIN_PATH:-}" ]; then
    echo "Missing arm-none-eabi-gcc. Install Arm GNU Toolchain and set PICO_TOOLCHAIN_PATH." >&2
    exit 1
fi
echo "Dependencies ready. Run cmake -B build -DCMAKE_BUILD_TYPE=Release"
