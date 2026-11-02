#!/usr/bin/env python3
"""Portable source/packaging checks; runtime behavior is covered by main.swift."""
from pathlib import Path
import json
import plistlib
import re

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "libhooker configurator"


def read(path):
    return path.read_text(encoding="utf-8")


def roothide_source(text):
    """Select the small set of conditional branches used by these app sources."""
    conditions = {
        "LHC_ROOTHIDE": True, "!LHC_ROOTHIDE": False,
        "LHC_TESTING": False, "!LHC_TESTING": True,
        "targetEnvironment(simulator)": False,
        "canImport(Darwin)": True, "canImport(Glibc)": False,
    }
    active = True
    stack = []
    output = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(("#ifdef ", "#ifndef ")):
            directive, name = stripped.split(maxsplit=1)
            assert name == "_64", f"Unknown C conditional: {name}"
            selected = directive == "#ifdef"
            stack.append((active, selected))
            active = active and selected
        elif stripped.startswith("#if "):
            condition = stripped[4:].strip()
            assert condition in conditions, f"Unknown conditional: {condition}"
            selected = conditions[condition]
            stack.append((active, selected))
            active = active and selected
        elif stripped.startswith("#elseif "):
            parent, taken = stack[-1]
            selected = not taken and conditions[stripped[8:].strip()]
            stack[-1] = (parent, taken or selected)
            active = parent and selected
        elif stripped == "#else":
            parent, taken = stack[-1]
            active = parent and not taken
            stack[-1] = (parent, True)
        elif stripped == "#endif":
            active, _ = stack.pop()
        elif active:
            output.append(line)
    assert not stack, "Unbalanced build conditions"
    return "\n".join(output)


store = read(APP / "Configs/LHUserDefaults.swift")
assert '"/var/mobile/Library/Preferences/org.coolstar.libhooker.plist"' in store
assert "Bundle.main.bundleIdentifier" not in store
assert "jailbreak(preferences)" in store
assert "normalizeRootFSPath(LHRootFSPath(path))" in store
assert "0o644" in store and "fsync(handle.fileDescriptor)" in store
assert "rename(temporary.path, url.path)" in store
assert "pending.isEmpty" in store and "private var registered" in store
assert "LHUserDefaults(loadImmediately: false)" in store
assert "func reloadAsync" in store and "requestedGeneration == self.generation" in store
assert "setProcessConfiguration" in store and "processChanges.removeAll()" in store
assert plistlib.loads((APP / "Info.plist").read_bytes())["UIRequiredDeviceCapabilities"] == ["arm64"]
launch = read(APP / "AppDelegate.swift")
assert '"tweaksEnabled": true' in launch and '"webProcessTweaks": true' in launch
assert ".synchronize()" not in launch
bridge = read(APP / "Data Providers/CoreServices/CoreServices.m")
assert "#import <roothide/roothide.h>" in bridge
assert "return jbroot(path);" in bridge and "return rootfs(path);" in bridge
assert ".jbroot-" not in bridge and "/var/jb" not in bridge

source_paths = [
    "Backend-Actions/Actions.swift", "Configs/DeviceInfo.swift",
    "UI/RootTableViewController.swift", "UI/Table View Cells/CellsCollection.swift",
    "UI/ConfigViewController.swift", "UI/LaunchServiceListView.swift",
    "UI/TweakCompatibilityList.swift", "Data Providers/launchd/launchd-list.m",
    "libhooker configurator-Bridging-Header.h",
]
selected = "\n".join(roothide_source(read(APP / path)) for path in source_paths)
for forbidden in ["URLSession", "giveMeRoot", "_os_alloc_once_table", "xpc_pipe_routine",
                  "launchdList()", "memPrefs", "userspaceReboot()", "ldRestart()", "OldABIEnabledSwitch()"]:
    assert forbidden not in selected, f"Legacy feature present in RootHide build: {forbidden}"
assert 'cell.defaultKey = "tweaksEnabled"' in selected
assert '"/System/Library/LaunchDaemons"' in selected
assert 'LHPaths.jailbreak("/Library/LaunchDaemons")' in selected
assert 'plist["Program"]' in selected and 'plist["ProgramArguments"]' in selected
assert 'cell.keys = [String(localizationKey: "Allow (0)"), String(localizationKey: "Deny (1)")]' in selected
assert "for (index, key) in keys.enumerated()" in selected and "at: index" in selected
assert "showPreferenceError()" in selected


def strings(path):
    pairs = re.findall(r'"((?:\\.|[^"\\])*)"\s*=\s*"((?:\\.|[^"\\])*)"\s*;', read(path))
    decoded = [(json.loads('"' + key + '"'), json.loads('"' + value + '"')) for key, value in pairs]
    assert len(decoded) == len(dict(decoded)), f"Duplicate localization key in {path.name}"
    return dict(decoded)


english = strings(APP / "UI/en.lproj/Localizable.strings")
chinese = strings(APP / "UI/zh-Hans.lproj/Localizable.strings")
visible_keys = set(re.findall(r'String\(localizationKey: "([^"\\]+)"\)', selected))
for key in visible_keys:
    assert key in english, f"Missing English help: {key}"
    assert key in chinese, f"Missing Simplified Chinese help: {key}"
assert chinese["Allow (0)"] == "允许" and chinese["Deny (1)"] == "禁止"
for path in [APP / "Info.plist", ROOT / "Entitlements.plist", ROOT / "Entitlements-roothide.plist"]:
    with path.open("rb") as handle:
        assert isinstance(plistlib.load(handle), dict)

build = read(ROOT / "scripts/build-roothide.sh")
for contract in ["IPHONEOS_DEPLOYMENT_TARGET=15.0", "ARCHS=arm64", "Architecture: iphoneos-arm64e",
                 "roothide, org.coolstar.libhooker (>= 1.6.9-23+roothide)", "-lroothide",
                 "LHC_ROOTHIDE", "IsTweakManager", "--root-owner-group",
                 'cp "$root/LICENSE" "$stage/usr/share/doc/$package/copyright"']:
    assert contract in build, f"Missing build contract: {contract}"
assert "chmod 4755" not in build and "curl " not in build and "scp " not in build
workflow_path = ROOT / ".github/workflows/main.yml"
if workflow_path.exists():
    workflow = read(workflow_path)
    assert "secrets." not in workflow and "curl " not in workflow and "Upload To Repo" not in workflow
project = read(ROOT / "libhooker configurator.xcodeproj/project.pbxproj")
sources = project.split("/* Begin PBXSourcesBuildPhase section */", 1)[1].split("/* End PBXSourcesBuildPhase section */", 1)[0]
assert sources.count("String+LocalizedHelpers.swift in Sources") == 1, "Only the existing Contrib localization helper should compile"
assert "only licensed for use on the Procursus bootstrap" in read(ROOT / "LICENSE")
assert "CoolStar" in read(ROOT / "README.md")
print(f"Static RootHide source, packaging, plist, and {len(visible_keys)} localized UI key checks passed")
