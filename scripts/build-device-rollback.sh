#!/bin/bash
# 编译当前生产核心与独立故障注入入口，不连接或安装到设备。
set -euo pipefail
cd "$(dirname "$0")/.."
source build-config.mk
: "${THEOS:?请设置 THEOS 为 roothide Theos 的安装目录}"
sdk_path="$THEOS/sdks/iPhoneOS${LH_IOS_SDK}.sdk"
test -d "$sdk_path" || { printf 'SDK 不存在：%s\n' "$sdk_path" >&2; exit 1; }
compiler=$(xcrun --find clang)
compiler_cxx=$(xcrun --find clang++)
signer=$(command -v ldid)
output_root="$PWD/worker/device-rollback"
mkdir -p "$output_root"
sources=()
# 读取当前核心分组，避免测试程序误用已经退出构建的旧引擎文件。
while IFS= read -r source; do sources+=("$source"); done < <(
    awk '/^LH_(SYMBOL|RELOCATION|MEMORY|OBJC|COMPAT)_FILES = / { for (i = 3; i <= NF; i++) print $i }' roothide.mk
)
(( ${#sources[@]} > 0 )) || { printf '未找到核心源码列表\n' >&2; exit 1; }
sources+=(libhooker/misc/misc.c tests/device-rollback.c)
for architecture in arm64 arm64e; do
    object_root="$output_root/$architecture"
    mkdir -p "$object_root"
    common=(-arch "$architecture" -isysroot "$sdk_path" "-miphoneos-version-min=$LH_IOS_MIN"
        -O2 -g -fblocks -fvisibility=hidden -DTHEOS -DLH_ROOTHIDE=1
        -Ilibhooker/include/apple -Ilibhooker/include -Ilibblackjack/include)
    objects=()
    for source in "${sources[@]}"; do
        object="$object_root/${source//\//_}.o"
        flags=("${common[@]}")
        if [[ "$source" == libhooker/mem/writeMem-roothide.c ]]; then flags+=(-Ddlsym=LHTestDlsym); fi
        if [[ "$source" == libhooker/func/funcHook.c ]]; then flags+=(-Dmach_vm_map=LHTestMachVMMap); fi
        if [[ "$source" == *.cpp ]]; then
            "$compiler_cxx" "${flags[@]}" -std=c++11 -c "$source" -o "$object"
        else
            "$compiler" "${flags[@]}" -std=gnu11 -c "$source" -o "$object"
        fi
        objects+=("$object")
    done
    executable="$output_root/DeviceRollback-$architecture"
    "$compiler_cxx" -arch "$architecture" -isysroot "$sdk_path" "-miphoneos-version-min=$LH_IOS_MIN" \
        "${objects[@]}" -lobjc -Wl,-headerpad,0x4000 -o "$executable"
    "$signer" -Stests/device-rollback.entitlements.plist "$executable"
    printf '已生成：%s\n' "$executable"
done
