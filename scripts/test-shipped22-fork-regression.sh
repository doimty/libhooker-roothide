#!/bin/bash
# Same production-source fixture against shipped22: it MUST reproduce the fork-SPI failure.
set -euo pipefail
cd "$(dirname "$0")/.."
baseline_commit=1c23789d6d6084452d7e3778babf577d816a0539
mkdir -p worker/host-tests
if ! git cat-file -e "$baseline_commit^{commit}" 2>/dev/null; then
    git fetch --no-tags --depth=1 origin "$baseline_commit"
fi
baseline_source="$PWD/worker/host-tests/dyldSyms-shipped22.c"
git show "$baseline_commit:libhooker/dyld/dyldSyms-modern.c" > "$baseline_source"
xcrun clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DLH_ROOTHIDE -Ilibhooker/include/apple -Ilibhooker/include -Ilibhooker/dyld \
    "-DLH_DYLD_TEST_SOURCE=\"$baseline_source\"" tests/image-lookup.c \
    libhooker/dyld/macho-symbols.c -o worker/host-tests/image-lookup-shipped22
set +e
(ulimit -c 0; worker/host-tests/image-lookup-shipped22) > worker/host-tests/shipped22-negative.log 2>&1
status=$?
set -e
if [[ "$status" -eq 0 ]]; then
    printf 'FAIL: shipped22 unexpectedly passed the fork-state regression\n' >&2
    exit 1
fi
if ! grep -F 'image && image->imageHeader == &target.header' worker/host-tests/shipped22-negative.log >/dev/null; then
    cat worker/host-tests/shipped22-negative.log >&2
    printf 'FAIL: negative control failed for an unrelated reason\n' >&2
    exit 1
fi
printf 'PASS negative control: actual shipped22 rejects a loaded image when fork SPI is available (exit=%s)\n' "$status"
