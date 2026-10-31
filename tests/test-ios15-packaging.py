#!/usr/bin/env python3
"""Source metadata and real after-stage recipe checks; no SDK or device needed.

The three runtime files are inert placeholders. Only Make's packaging recipe
runs in a fresh temporary tree; no dylib or maintainer script is executed.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def config_fields(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines()
                if line and not line.startswith("#"))


def control_fields(path):
    return dict(line.split(": ", 1) for line in path.read_text().splitlines()
                if ": " in line and not line.startswith(" "))


class IOS15Packaging(unittest.TestCase):
    def test_deployment_and_firmware_floor(self):
        config = config_fields(ROOT / "build-config.mk")
        control = control_fields(ROOT / "control")
        self.assertEqual(config["LH_IOS_MIN"], "15.0")
        self.assertIn("firmware (>= 15.0)", control["Depends"])
        self.assertIn("firmware (<< 18.0)", control["Depends"])
        self.assertIn("roothide", control["Depends"].split(", "))
        self.assertEqual(control["Architecture"], "iphoneos-arm64e")
        for version, accepted in (("14.8", False), ("15.0", True),
                                  ("15.1.1", True), ("16.0", True),
                                  ("17.0", True), ("18.0", False)):
            with self.subTest(version=version):
                lower = subprocess.run(["dpkg", "--compare-versions", version,
                                        "ge", config["LH_IOS_MIN"]]).returncode
                upper = subprocess.run(["dpkg", "--compare-versions", version,
                                        "lt", "18.0"]).returncode
                self.assertEqual(lower == 0 and upper == 0, accepted)

    def test_preserve_modern_backend_and_dual_architecture(self):
        makefile = (ROOT / "roothide.mk").read_text()
        self.assertIn("TARGET = iphone:clang:$(LH_IOS_SDK):$(LH_IOS_MIN)", makefile)
        self.assertRegex(makefile, r"(?m)^ARCHS = arm64 arm64e$")
        symbols = re.search(r"(?m)^LH_SYMBOL_FILES = (.*)$", makefile).group(1).split()
        self.assertIn("libhooker/dyld/dyldSyms-modern.c", symbols)
        self.assertNotIn("libhooker/dyld/dyldSyms.c", symbols)
        memory = re.search(r"(?m)^LH_MEMORY_FILES = (.*)$", makefile).group(1).split()
        self.assertIn("libhooker/mem/writeMem-roothide.c", memory)
        self.assertIn('"-miphoneos-version-min=$LH_IOS_MIN"',
                      (ROOT / "scripts/build-device-rollback.sh").read_text())
        verifier = (ROOT / "scripts/verify-package.py").read_text()
        self.assertIn('minimum_ios = build_config["LH_IOS_MIN"]', verifier)
        self.assertIn('"TweakLoader.dylib"', verifier)
        self.assertIn('"TweakInject.dylib"', verifier)

    def check_stage(self, engine):
        with tempfile.TemporaryDirectory(prefix="lh-ios15-stage-") as directory:
            temporary = Path(directory)
            theos = temporary / "theos"
            makefiles = theos / "makefiles"
            makefiles.mkdir(parents=True)
            (makefiles / "common.mk").write_text("# No SDK/compile in this fixture.\n")
            (makefiles / "library.mk").write_text("# Only after-stage runs.\n")
            stage = temporary / "stage"
            library = stage / "usr/lib"
            # Theos 会把 layout 内容放进暂存目录；模拟已存在的相对链接，避免掩盖重复创建的失败。
            for name in ("TweakLoader.dylib", "TweakInject.dylib"):
                path = library / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.symlink_to("libhooker/libinjector.dylib")
            for name in (f"{engine}.dylib", "libhooker/libinjector.dylib",
                         "libhooker/MobileSafety.dylib", f"{engine}.a"):
                path = library / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture-not-a-runtime-binary\n")
            command = ["make", "--no-print-directory", "-f", "roothide.mk",
                       "after-stage", f"THEOS={theos}",
                       f"THEOS_MAKE_PATH={makefiles}",
                       f"THEOS_STAGING_DIR={stage}", f"LH_ENGINE_NAME={engine}"]
            result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            injector = library / "libhooker/libinjector.dylib"
            for name in ("TweakLoader.dylib", "TweakInject.dylib"):
                link = library / name
                self.assertTrue(link.is_symlink(), str(link))
                self.assertEqual(os.readlink(link), "libhooker/libinjector.dylib")
                self.assertEqual(link.resolve(strict=True), injector.resolve(strict=True))
            for name in ("libjoker", "libhooker", "libblackjack", "libsubstrate", "libellekit"):
                if name != engine:
                    link = library / f"{name}.dylib"
                    self.assertEqual(os.readlink(link), f"{engine}.dylib")
                    self.assertEqual(link.resolve(strict=True), (library / f"{engine}.dylib").resolve())
            framework = stage / "Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate"
            self.assertEqual(framework.resolve(strict=True), (library / f"{engine}.dylib").resolve())
            self.assertTrue((library / "TweakInject").is_dir())
            self.assertFalse((library / f"{engine}.a").exists())
            # macOS 上 TMPDIR 位于 /var/folders，realpath 会解析成 /private/var，
            # 因此基准目录也必须先解析，否则所有合法相对链接都会被误判为越界。
            resolved_stage = stage.resolve()
            for path in stage.rglob("*"):
                if path.is_symlink():
                    self.assertFalse(os.path.isabs(os.readlink(path)), str(path))
                    self.assertTrue(path.resolve(strict=True).is_relative_to(resolved_stage))

    def test_libjoker_real_stage(self):
        self.check_stage("libjoker")

    def test_libhooker_real_stage(self):
        self.check_stage("libhooker")


if __name__ == "__main__":
    unittest.main(verbosity=2)
