# Configuration utility for libhooker (iOS 12 and higher)

## Languages / Toolkits

This is primarily written in Swift and uses ~~SwiftUI~~ UIKit for its interface

RIP SwiftUI for being buggy :( -- we tried

## Credits

* libhooker is written by CoolStar
* Thanks to Morpheus_____ for documenting launchd
* Contains code from Anemone's configuration utility
* Contains a lightweight version of Sileo's backend

* License

Licensed under a modified BSD license for use on the Procursus bootstrap. See: [LICENSE](LICENSE).

This product includes software developed by CoolStar.

## Optional RootHide Procursus addon

This adaptation of `coolstar/libhooker-configurator` (upstream main at `167560fe`)
provides process and per-tweak configuration for the RootHide libhooker engine.
The optional package requires `roothide`, iOS 15 or newer, and
`org.coolstar.libhooker (>= 1.6.9-23+roothide)`. Engine 22 does not implement this
configuration contract. The upstream [Makefile](Makefile) remains the rootful
build; [build-roothide.mk](build-roothide.mk) is a separate addon entry point.

The RootHide interface includes the global Tweaks and webpage switches, default
process policy, SpringBoard, applications, and daemons. It hides the old ABI and
memory-backend compatibility controls. Application discovery still uses
LaunchServices. Daemons are discovered from `/System/Library/LaunchDaemons` and
`jbroot("/Library/LaunchDaemons")`, using `Program` or the first
`ProgramArguments` item. The list includes installed services even when stopped;
services with the same executable share one configuration. Directory failures,
unreadable plists, and missing lists are reported in the interface. Relative
executable paths cannot be configured and are counted as skipped.

Settings save as they are edited. Opening the app or a configuration screen only
reads preferences and registers in-memory defaults. The Global Tweaks switch
writes a preference and needs no setuid helper. RootHide uses no helper or app
commands for respring, ldrestart, or userspace reboot. The Apply dialog instructs
the user to restart the target app or respring SpringBoard. Use Dopamine for a
userspace reboot. Running processes do not reload these settings live.

### Changes in 1.4+roothide.2

- Package versions and tweak-directory scans load off the main thread; getters do not start repeated I/O. Package refreshes coalesce, keep the last good snapshot on read failure, and update the visible rows on completion.
- Opening the app registers defaults without reading or writing the preference file. Screens explicitly read a snapshot in the background and keep editing disabled until that read completes. Returning to a screen, foreground activation and pull-to-refresh reload external changes.
- A delayed read started before a save is discarded using a generation check. Single-process saves and inheritance deletion apply to the latest on-disk configuration tree, preserving other processes edited externally. Atomic mode/flush/rename and failed-edit rollback remain unchanged. Individual saves remain synchronous; no write durability was traded for speed.
- Process list errors include at most five concrete details plus a total count. Hidden apps remain filtered; relative daemon executable paths remain unconfigured rather than guessed. Deterministic plist ordering stabilizes shared-executable labels.
- The app declares arm64 capabilities. No new engine toggles, helpers, restart commands, or device actions are introduced.

### Build on macOS

Use Xcode with its command-line tools, `ldid`, `dpkg-deb`, and the pinned RootHide
Theos checkout `88506b2` including its submodules. The UIKit app uses the selected
Xcode's platform iPhoneOS SDK with deployment target 15.0; it does not use the
patched SDK intended for Theos engine builds. The build does not download tools,
sign with Apple credentials, install packages, or operate a device.

```sh
export THEOS=/absolute/path/to/roothide-theos
export LHC_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
bash tests/run.sh
make -f build-roothide.mk package
```

When the parent repository vendors this checkout under `manager/`, use:

```sh
bash manager/tests/run.sh
make -C manager -f build-roothide.mk package
```

The script passes `LHC_ROOTHIDE` to both Swift and Objective-C, deployment target
15.0 and `ARCHS=arm64` to `xcodebuild`. It includes `$THEOS/vendor/include` and
`$THEOS/include` and links `-lroothide` from
`$THEOS/vendor/lib/iphone/roothide`. That library's RootHide install name is
resolved through the app's `.jbroot` link supplied by the RootHide installer.
The C/Objective-C bridge stays in the existing CoreServices source known to the
Xcode project. It calls the actual `jbroot` / `rootfs` APIs from
`<roothide/roothide.h>`; it does not scan for randomized roots.

Default outputs:

- App: `.build/roothide/Build/Products/Release-iphoneos/libhooker.app`.
- Package: `packages/org.coolstar.libhooker-configurator_1.4+roothide.2_iphoneos-arm64e.deb`.
- Package app payload: `/Applications/libhooker.app`, signed by `ldid` with
  [Entitlements-roothide.plist](Entitlements-roothide.plist). `IsTweakManager` is
  set on the packaged app. No helper, setuid file, or maintainer script is added.
- License and credits: `/usr/share/doc/org.coolstar.libhooker-configurator/copyright`
  and `README.md` inside the package.

`LHC_BUILD_DIR`, `LHC_OUTPUT_DIR`, `LHC_PACKAGE_VERSION`, and `LHC_SDK` may be set
for CI. The package architecture is `iphoneos-arm64e`, while the UIKit executable
uses stable arm64 code. The default source Bundle ID is
`org.coolstar.libhooker-configurator`; changing it does not move engine settings.
The parent repository builds and uploads artifacts only; it does not retain the upstream external repository uploader.

### Preference contract with engine 23

The fixed device path is
`jbroot("/var/mobile/Library/Preferences/org.coolstar.libhooker.plist")`, independent
of the app Bundle ID. The simulator uses an Application Support file instead.
Writes serialize a binary plist to a unique temporary file beside the target,
set mode `0644`, flush it, then atomically rename it over the target. Registration
is never persisted by itself. Saves preserve unrelated top-level keys from the
latest on-disk file. Read, serialization, permission, and rename failures are
visible; a failed edit reverts and leaves the previous file intact. An invalid
existing plist is never silently replaced.

The engine contract has top-level `tweaksEnabled` and `webProcessTweaks` booleans,
both defaulting to `true`, plus `tweakconfigs` with `default`, `bundles`, and
`paths` records. One whole process record wins in this order:

1. Matching application Bundle ID.
2. Matching logical executable path, even when there is a Bundle ID but no
   matching bundle record.
3. The `default` record.
4. Built-in field defaults when there is no record or a field is absent.

Fields are not merged from lower-priority records. A record contains
`enableTweaks` (default `true`), `customConfig` (default `false`), `allowDeny`
(default `1`), and `tweakConfigs` (a dictionary of exact `*.dylib` filenames to
selection booleans). With `customConfig=false`, only a tweak's existing Filter
applies. With `customConfig=true`, **Allow (0)** permits only selected tweaks and
**Deny (1)** blocks selected tweaks. Those restrictions never bypass a tweak's
Filter. Disabling a process, the global switch, or the webpage gate skips the
applicable injection before list selection. Any engine `IsTweakManager`
exception applies only to per-tweak restrictions.

Executable keys use `rootfs()` and remove its exact `/rootfs/` prefix for system
paths. Physical randomized bootstrap paths are not persisted. Application rows
retain their logical executable path for fallback lookup; daemon rows are keyed
by their logical executable. **Inherit** deletes the specific bundle or path
record, preserving the default and allowing the next matching record to apply.
Saved filenames for temporarily uninstalled tweaks remain editable and are
preserved by later saves.

### Validation and remaining limits

[tests/main.swift](tests/main.swift) compiles the real Foundation preference and
path code without UIKit. Its 18 tests cover deferred and coalesced reads, main-queue completion, stale-read rejection, failure/retry, per-process merge/reset, no writes on open or registration,
mode `0644`, atomic failure preservation and cleanup, malformed and
non-dictionary plists, serialization errors, independent top-level edits,
current path resolution at save, whole-record precedence, inheritance removal,
and exact filename / Allow-Deny round trips. The native RootHide bridge needs the
iOS build and RootHide runtime; the standalone tests exercise the logical path
normalization after that API returns.

[tests/check_contract.py](tests/check_contract.py) is a portable static check for
the fixed domain, RootHide build branches, absence of legacy helpers / IPC /
network calls in those branches, package metadata, plist syntax, localization
coverage, and a single compiled localization helper. Both copies of the
upstream localization helper remain on disk; only the existing Contrib copy is
referenced by the Xcode Sources build phase, so no duplicate-file refactor is
needed.

```sh
python3 tests/check_contract.py
bash -n scripts/build-roothide.sh tests/run.sh
git diff --check
```

The implementation host is Windows and has no Xcode, iOS SDK compiler, or Swift
compiler. Portable static and shell-syntax checks were run locally; native
Foundation tests and the UIKit build must pass in the parent's macOS CI. No
installation or test on the iPhone14,3 / iOS 15.1.1 device has been performed.
App registration, `.jbroot` linking, readable daemon access, and actual injection
behavior remain device integration checks for the RootHide installer and engine.

