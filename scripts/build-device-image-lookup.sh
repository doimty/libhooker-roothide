#!/bin/bash
# Build a standalone symbol-lookup probe; never installs or starts it on a device.
set -euo pipefail
cd "$(dirname "$0")/.."
source build-config.mk
: "${THEOS:?Set THEOS to the roothide Theos directory}"
sdk="$THEOS/sdks/iPhoneOS${LH_IOS_SDK}.sdk"
test -d "$sdk"
mkdir -p worker
xcrun clang -Wall -Wextra -Werror -arch arm64 -arch arm64e \
    -isysroot "$sdk" "-miphoneos-version-min=$LH_IOS_MIN" \
    tests/device-image-lookup.c -o worker/lh-image-probe
ldid -S worker/lh-image-probe
xcrun lipo -archs worker/lh-image-probe
