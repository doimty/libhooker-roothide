#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
command -v swiftc >/dev/null || { printf 'Swift/Foundation compiler required (macOS or Linux)\n' >&2; exit 1; }
temp_root="$(cd "${TMPDIR:-/tmp}" && pwd)"
test_dir="$(mktemp -d "$temp_root/lhc-tests.XXXXXX")"
cleanup() {
    case "$test_dir" in "$temp_root"/lhc-tests.*) rm -rf -- "$test_dir" ;; esac
}
trap cleanup EXIT
swiftc -swift-version 5 -D LHC_TESTING \
    "$root/libhooker configurator/Configs/LHUserDefaults.swift" \
    "$root/tests/main.swift" -o "$test_dir/preferences-tests"
"$test_dir/preferences-tests"
python3 "$root/tests/check_contract.py"
