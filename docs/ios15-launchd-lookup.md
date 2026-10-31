# iOS 15.1.1 launchd crash investigation

## Confirmed failure

The supplied report describes iPhone14,3, iOS 15.1.1 (19B81), arm64e, SIGABRT in `fix__iosConnect + 84` inside `libroothider.dylib`.

Offline disassembly of Dopamine build `c2b9cfe` gives the following arm64e addresses. The standalone bundled dylib and the copy in basebin.tar have the same code addresses and UUID `F405AD0C-1A46-3AA6-8E7D-BAB25BBB96AE` (their whole-file hashes differ).

| Evidence | Value |
| --- | --- |
| Reported libroothider load address | `0x101248000` |
| Reported fix__iosConnect frame | `0x10125db68` |
| Relative frame address | `0x15b68` |
| `_fix__iosConnect` symbol address | `0x15b14` |
| Instruction at `+80` | `bl ___assert_rtn` |
| Return address at `+84` | `0x15b68` |
| Assertion string | `IOSurfaceImage != NULL` |
| Source location supplied to assert | `launchd.m:160` |

This is the first image lookup assertion. The failing call has not yet reached `MSFindSymbol`, `IOServiceOpen` or `IOServiceClose`. The binary has a normal `LC_LOAD_DYLIB` dependency on IOSurface and a weak framework dependency on CydiaSubstrate. IOSurface and libjoker are also present in the crash report's image list.

The crash report does not record the dylib UUID or the `dlopen` error string, so address/source agreement is strong evidence of the installed build, not an independent identity proof. It does establish which assertion this matching build reaches. ResetCounter records a reset and is not a kernel-panic cause report.

## Confirmed source defect and fix

On systems without `_dyld_get_dlopen_image_header`, the resolver previously required `realpath(requestedPath)` and `realpath(dyldImageName)` to succeed before accepting an image. A dyld-shared-cache image need not have a standalone file at that path. An already loaded image could therefore be rejected even after successful `RTLD_NOLOAD` lookup.

The fix first compares complete dyld image names to the requested path, without filesystem lookup. Canonical filesystem matching remains as a fallback for aliases. The resolver continues to use NOLOAD, LOCAL and FIRST flags and keeps its own handle until validation/close; it does not load an unrequested new library or match by basename.

The original crash report did not log filesystem or dyld return values. A subsequent USB Frida session on the connected iOS 15.1.1 device reproduced `realpath` ENOENT and a NULL image in baseline 21, while candidate 22 returned the correct image and both private symbols. The same comparison passed without an attached interception script. See [device validation](ios15-frida-validation.md) for the exact scope and cleanup record. This establishes the targeted query fix, not complete engine-replacement or reboot validation.

## Verification

`tests/image-lookup.c` executes the production image-open code and Mach-O slide parser with mocked dyld and filesystem APIs. It covers missing header SPI, cache-only image paths, same-basename dependencies, disk aliases, SPI success/null fallback, unloaded images, malformed headers and handle cleanup. `scripts/test-host.sh` runs it with AddressSanitizer and UndefinedBehaviorSanitizer along with the existing host tests. CI now runs that host suite in addition to packaging tests and package metadata verification.

## Standalone device diagnostic

Build with:

```sh
bash scripts/build-device-image-lookup.sh
```

The result is `worker/lh-image-probe`, with arm64 and arm64e slices. It is separate from the deb and is not installed or executed automatically. In a normal shell process, pass an absolute path to the engine library you want to examine:

```sh
/path/to/lh-image-probe /absolute/path/to/libjoker.dylib
```

It loads IOSurface and the engine into its own process, prints the filesystem lookup, loaded-image header and the two private-symbol lookup results, then closes the handles. It does not load the injector/safe-mode library, install hooks, dereference or modify the returned globals, call IOServiceOpen/Close, touch launchd, or request a reboot. Loading a library still executes its constructors; use it only with the intended engine, not arbitrary dylibs. A successful probe establishes image/symbol behavior in that process, not complete injection or launchd validation.

## Other review findings

- Do not delete Conflicts/Replaces merely to co-install ElleKit: the packages intentionally overlap loader, framework and alias paths. Fixing engine replacement and designing optional co-existence are different changes.
- The normal Dopamine launchdhook build includes `roothider_common.c`, which exports the two substrate memory backend functions. Their absence in launchd has not been demonstrated.
- Current shared-cache wide-entry selection agrees with Apple's dyld implementation. No evidence justifies selecting cache layout purely by iOS version.
- The symbol parser signs instruction symbols and leaves data storage addresses raw; blanket PAC-signing of `__iosService`/`__iosConnect` would be incorrect.
- OldABI is optional in the upstream loader; its absence is not established as a regression here.
- ElleKit Swift ABI consumers are outside the current C-wrapper compatibility surface. Export-name/package checks do not prove compatibility for those tweaks.
- A separate indexed dyld-list lookup race remains: name/header calls can observe different indices when an unrelated image unloads. This report does not show that race caused the supplied crash. It is not changed by the focused filesystem-lookup fix.
- Disabling tweak injection selects Dopamine's bundled fallback framework on subsequent activation. Recovery in that mode does not validate replacement-engine startup.
