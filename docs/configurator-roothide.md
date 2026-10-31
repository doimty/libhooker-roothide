# RootHide Configurator integration

This optional add-on ports CoolStar's libhooker Configurator to the Procursus-based RootHide bootstrap. The adapted application and the injector use one explicit file:

```text
jbroot("/var/mobile/Library/Preferences/org.coolstar.libhooker.plist")
```

The UI does not install a privileged helper, replace package ownership, or create preferences when first opened. Saving an edit writes an atomic, mobile-owned, readable plist. The GUI has a separate package and requires the configuration-enabled engine `1.6.9-23+roothide` or later; engine22 never consumed these settings.

## Effective policy

The root dictionary contains `tweaksEnabled` and `webProcessTweaks` booleans, both defaulting to true. `tweaksEnabled` is the new shared global switch; it replaces the rootful marker-file action in this RootHide UI. The WebKit switch applies only to WebContent identity, not every process containing "WebKit" in its name.

The existing Configurator record format is retained:

```text
tweakconfigs:
  default: Record
  bundles:
    com.example.application: Record
  paths:
    /usr/libexec/exampled: Record

Record:
  enableTweaks: Bool            # default true
  customConfig: Bool            # default false
  allowDeny: Integer            # 0 Allow / 1 Deny, default 1
  tweakConfigs:
    Example.dylib: Bool         # true = selected, false/missing = unselected
```

A bundle record wins, otherwise a matching full logical executable path wins, otherwise the `default` record wins. One whole record is selected. Fields are not merged between records. This is an explicitly defined compatibility contract based on the public application's serialized data; the original native reader was not available for inspection.

- `enableTweaks=false` stops the process's plugin scan, including managers.
- `customConfig=false` applies only the existing tweak metadata Filter.
- `customConfig=true` applies an additional list restriction after that Filter. An Allow list admits only selected names. A Deny list excludes selected names. Allow never force-loads a tweak whose metadata does not match.
- Choose the UI's Use Default option to remove a process record and inherit the default record. Merely switching off Custom Tweak Selection does not remove that process override.
- Manager plugins retain their existing metadata/version checks and run before ordinary tweaks. They bypass only the per-tweak list restriction. Global/process disable, safe mode, integrity gates, and the requirement that at least one ordinary tweak remain still apply.
- Unknown/stale preference names do not create load paths. Only actual `.dylib` directory candidates are considered.

The process reads one immutable bounded snapshot before scanning and before any plugin `dlopen`. Settings apply to a process on its next start. They do not unload libraries from a running process. Restart the relevant application; SpringBoard/daemon changes may require an explicit restart through Dopamine. Saving never initiates a restart.

## Paths and failure handling

Runtime and UI convert physical RootHide executable paths with `rootfs()`. Its exact `/rootfs/` wrapper for native system paths is removed, leaving shared logical identities such as `/System/Library/CoreServices/SpringBoard.app/SpringBoard`. Paths are full paths, never basename-only. Bundle IDs remain stable across app container changes.

Missing, unreadable, corrupt or non-dictionary preferences preserve the prior loader behavior. Reading rejects symlink config files, non-regular files and files larger than 4 MiB. Typed policy parsing never coerces integers into booleans. A malformed optional custom list drops that restriction while still honoring a valid process/global disable. This fallback is recovery behavior for malformed settings, not a substitute for correctly saved policy.

## Verification and limits

`tests/config-policy.c` uses actual CoreFoundation values to check precedence, whole records, malformed types, WebContent identity and filename membership. `tests/injector-config.c` executes the actual loader with inert file fixtures and a `dlopen` recorder, covering the real selection and ordering, manager restrictions, snapshot timing and safety/integrity gates. No runtime plugin is loaded by these host tests.

The existing engine22 image-lookup fix and its tests remain in place. This integration changes plugin selection, not the Hook backend or the IOSurface fix. Host/build/package success must remain separate from later user-authorized app/UI/device acceptance. The currently functioning installed engine22 is not changed automatically.

The legacy alternate-memory-backend and OldABI controls are not exposed as supported features. They do not have an equivalent implementation in this engine and must not appear to work merely because a preference was saved.

## Attribution

The application is derived from `coolstar/libhooker-configurator` commit `167560fe20964c8ff3567af6818977cb31ade3f0`. This product includes software developed by CoolStar. Its original modified BSD license is retained with the application; that license restricts use to the Procursus bootstrap. The injector and engine retain their own existing license notices.
