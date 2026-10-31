#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
: "${THEOS:?Set THEOS to the pinned RootHide Theos checkout}"
THEOS="$(cd "$THEOS" && pwd)"
sdk="${LHC_SDK:-$THEOS/sdks/iPhoneOS17.3.1.sdk}"
derived="${LHC_BUILD_DIR:-$root/.build/roothide}"
output="${LHC_OUTPUT_DIR:-$root/packages}"
version="${LHC_PACKAGE_VERSION:-1.4+roothide.1}"
package=org.coolstar.libhooker-configurator

for tool in xcodebuild xcrun ldid dpkg-deb; do
    command -v "$tool" >/dev/null || { printf 'Required build tool missing: %s\n' "$tool" >&2; exit 1; }
done
[[ -d "$sdk" ]] || { printf 'SDK not found: %s (set LHC_SDK)\n' "$sdk" >&2; exit 1; }
[[ -f "$THEOS/vendor/include/roothide/roothide.h" || -f "$THEOS/include/roothide/roothide.h" ]] || {
    printf 'RootHide headers missing from THEOS\n' >&2; exit 1;
}
[[ -f "$THEOS/vendor/lib/iphone/roothide/libroothide.tbd" ]] || {
    printf 'RootHide libroothide.tbd missing from THEOS/vendor/lib/iphone/roothide\n' >&2; exit 1;
}
# Reject malformed control-file values before writing the package metadata.
[[ "$version" =~ ^[0-9][A-Za-z0-9.+:~\-]*$ ]] || { printf 'Invalid package version\n' >&2; exit 1; }

xcodebuild -project "$root/libhooker configurator.xcodeproj" \
    -scheme 'libhooker configurator' -configuration Release \
    -sdk "$sdk" -destination 'generic/platform=iOS' -derivedDataPath "$derived" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
    IPHONEOS_DEPLOYMENT_TARGET=15.0 ARCHS=arm64 VALID_ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
    PRODUCT_BUNDLE_IDENTIFIER=org.coolstar.libhooker-configurator \
    ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES=NO \
    'SWIFT_ACTIVE_COMPILATION_CONDITIONS=$(inherited) LHC_ROOTHIDE' \
    'GCC_PREPROCESSOR_DEFINITIONS=$(inherited) LHC_ROOTHIDE=1' \
    "HEADER_SEARCH_PATHS=\$(inherited) \"$root/apple_include\" \"$THEOS/vendor/include\" \"$THEOS/include\"" \
    "LIBRARY_SEARCH_PATHS=\$(inherited) \"$THEOS/vendor/lib/iphone/roothide\"" \
    'OTHER_LDFLAGS=$(inherited) -lroothide'

app="$derived/Build/Products/Release-iphoneos/libhooker.app"
[[ -d "$app" ]] || { printf 'Built app missing: %s\n' "$app" >&2; exit 1; }
# Stage in a fresh, known temporary directory; never remove caller-provided paths.
temp_root="${TMPDIR:-/tmp}"
temp_root="$(cd "$temp_root" && pwd)"
stage="$(mktemp -d "$temp_root/lhc-roothide.XXXXXX")"
cleanup() {
    case "$stage" in "$temp_root"/lhc-roothide.*) rm -rf -- "$stage" ;; esac
}
trap cleanup EXIT
mkdir -p "$stage/Applications" "$stage/DEBIAN" "$stage/usr/share/doc/$package" "$output"
cp -R "$app" "$stage/Applications/libhooker.app"
staged_app="$stage/Applications/libhooker.app"
[[ ! -e "$staged_app/giveMeRoot" ]] || { printf 'Unexpected legacy root helper in app\n' >&2; exit 1; }
/usr/libexec/PlistBuddy -c 'Add :IsTweakManager bool true' "$staged_app/Info.plist"
executable="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$staged_app/Info.plist")"
[[ "$executable" == libhooker ]] || { printf 'Unexpected app executable\n' >&2; exit 1; }
chmod -R u=rwX,go=rX "$stage"
chmod 0755 "$staged_app/$executable"
ldid -S"$root/Entitlements-roothide.plist" "$staged_app/$executable"
cp "$root/LICENSE" "$stage/usr/share/doc/$package/copyright"
cp "$root/README.md" "$stage/usr/share/doc/$package/README.md"

cat > "$stage/DEBIAN/control" <<CONTROL
Package: $package
Name: libhooker Configurator (RootHide)
Version: $version
Architecture: iphoneos-arm64e
Priority: optional
Section: Tweaks
Maintainer: RootHide addon maintainers
Author: CoolStar
Depends: firmware (>= 15.0), firmware (<< 18.0), roothide, org.coolstar.libhooker (>= 1.6.9-23+roothide)
Description: Optional process and tweak configuration for libhooker on RootHide Procursus.
 This product includes software developed by CoolStar.
 Licensed for the Procursus bootstrap only.
CONTROL
chmod 0644 "$stage/DEBIAN/control"
xcrun lipo -archs "$staged_app/$executable"
xcrun otool -L "$staged_app/$executable"
deb="$output/${package}_${version}_iphoneos-arm64e.deb"
dpkg-deb --root-owner-group -Zxz --build "$stage" "$deb"
printf 'Built %s\n' "$deb"
