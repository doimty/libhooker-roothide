#!/usr/bin/env python3
"""检查最终 deb 的元数据、链接布局、架构及公开兼容接口。"""
from pathlib import Path
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

if len(sys.argv) not in (2, 3):
    raise SystemExit(f"Usage: {sys.argv[0]} package.deb [libjoker|libhooker]")
engine_name = sys.argv[2] if len(sys.argv) == 3 else "libjoker"
if engine_name not in ("libjoker", "libhooker"):
    raise SystemExit("Engine must be libjoker or libhooker")
alias_name = "libhooker" if engine_name == "libjoker" else "libjoker"
package = Path(sys.argv[1]).resolve()
dpkg = shutil.which("dpkg-deb")
if not dpkg:
    raise SystemExit("dpkg-deb is required")
metadata = subprocess.check_output([dpkg, "-f", str(package)], text=True)
fields = dict(line.split(": ", 1) for line in metadata.splitlines() if ": " in line and not line.startswith(" "))
build_config = dict(line.split("=", 1) for line in Path("build-config.mk").read_text().splitlines()
                    if line and not line.startswith("#"))
minimum_ios = build_config["LH_IOS_MIN"]
assert fields["Package"] == "org.coolstar.libhooker"
assert fields["Architecture"] == "iphoneos-arm64e"
assert "ellekit" in fields["Conflicts"].split(", ")
assert "ellekit" in fields["Replaces"].split(", ")
assert "ellekit (= 1.2-3)" in fields["Provides"]
assert fields["Version"] in package.name
assert f"firmware (>= {minimum_ios})" in fields["Depends"]
Path("worker").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="verify-package-", dir="worker") as directory:
    root = Path(directory).resolve()
    subprocess.run([dpkg, "-x", str(package), str(root)], check=True)
    subprocess.run([dpkg, "-e", str(package), str(root / "DEBIAN")], check=True)
    (root / ".jbroot").symlink_to(".")  # 模拟 bootstrap 中已存在的根链接。
    for path in root.rglob("*"):
        if path.is_symlink():
            assert not os.path.isabs(os.readlink(path)), path
            assert path.resolve().exists() and path.resolve().is_relative_to(root), path
    engine = root / f"usr/lib/{engine_name}.dylib"
    assert engine.is_file() and not engine.is_symlink()
    for name in (f"usr/lib/{alias_name}.dylib", "usr/lib/libellekit.dylib", "usr/lib/libsubstrate.dylib",
                 "usr/lib/libblackjack.dylib", "Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate"):
        assert (root / name).is_symlink() and (root / name).resolve() == engine
    injector = root / "usr/lib/libhooker/libinjector.dylib"
    for name in ("TweakLoader.dylib", "TweakInject.dylib"):
        link = root / "usr/lib" / name
        assert link.is_symlink() and os.readlink(link) == "libhooker/libinjector.dylib", name
        assert link.resolve() == injector, name
    assert not (root / "usr/lib/libjoker.a").exists() and not (root / "usr/lib/libhooker.a").exists()
    for name in (f"{engine_name}.dylib", "libhooker/libinjector.dylib", "libhooker/MobileSafety.dylib"):
        binary = root / "usr/lib" / name
        architectures = subprocess.check_output(["xcrun", "lipo", "-archs", str(binary)], text=True)
        assert set(architectures.split()) == {"arm64", "arm64e"}
        load_commands = subprocess.check_output(["xcrun", "dyld_info", "-load_commands", str(binary)], text=True)
        assert re.findall(r'min-OS:\s+(\S+)', load_commands) == [minimum_ios, minimum_ios], name
    exports = subprocess.check_output(["xcrun", "dyld_info", "-exports", str(engine)], text=True)
    identity = f"@loader_path/.jbroot/usr/lib/{engine_name}.dylib"
    commands = subprocess.check_output(["xcrun", "dyld_info", "-load_commands", str(engine)], text=True)
    ids = re.findall(r'cmd: LC_ID_DYLIB\s+cmdsize:[^\n]+\s+name: "([^"]+)"', commands)
    assert ids == [identity, identity], ids
    dependencies = subprocess.check_output(["xcrun", "dyld_info", "-linked_dylibs",
        str(root / "usr/lib/libhooker/libinjector.dylib")], text=True)
    assert dependencies.count(identity) == 2 and f"/{alias_name}.dylib" not in dependencies
    required = ("LHHookFunctions", "LHPatchMemory", "LHExecMemory", "LHOpenImage", "LHFindSymbols", "LHCloseImage",
                "LHWriteMemory", "LHVersion", "LHStrError",
                "LBHookMessage", "MSHookFunction", "MSHookMessageEx", "MSGetImageByName", "MSFindSymbol",
                "MSCloseImage", "MSHookClassPair", "MSHookMemory", "MSHookIvar", "EKHookFunction",
                "EKHookMemoryRaw", "EKEnableThreadSafety")
    for symbol in required:
        assert exports.count("_" + symbol + "\n") == 2, symbol
    exported_names = set(re.findall(r'^[ \t]*0x[0-9A-Fa-f]+[ \t]+(_\S+)$', exports, re.M))
    assert exported_names == {"_" + symbol for symbol in required}, exported_names
    safety_exports = subprocess.check_output(["xcrun", "dyld_info", "-exports",
        str(root / "usr/lib/libhooker/MobileSafety.dylib")], text=True)
    assert safety_exports.count("_LHCrashGuardIsReady\n") == 2
    assert safety_exports.count("_LHIsSafeModeRequested\n") == 2
    for name in ("preinst", "postinst"):
        assert (root / "DEBIAN" / name).stat().st_mode & 0o111
        subprocess.run(["bash", "-n", str(root / "DEBIAN" / name)], check=True)
    assert (root / "usr/share/doc/libhooker/LICENSE").is_file()
    assert (root / "usr/share/doc/libhooker/ELLEKIT-LICENSE").is_file()
    # 发行包仅包含运行库、许可证和操作说明，防止历史报告或构建产物混入。
    runtime_files = {f"usr/lib/{engine_name}.dylib", "usr/lib/libhooker/libinjector.dylib",
                     "usr/lib/libhooker/MobileSafety.dylib"}
    document_files = {"usr/share/doc/libhooker/" + name
                      for name in ("LICENSE", "ELLEKIT-LICENSE")}
    # 按归档本身校验，避免 Finder 在临时解包目录生成 .DS_Store 导致误报。
    payload = subprocess.check_output([dpkg, "--fsys-tarfile", str(package)])
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:") as archive:
        payload_files = {Path(member.name).as_posix() for member in archive.getmembers()
                         if member.isfile() or member.islnk()}
    assert payload_files == runtime_files | document_files, payload_files
print(f"PASS package metadata, runtime-only payload, {engine_name} identity, injector dependencies, aliases, architectures and exports")
