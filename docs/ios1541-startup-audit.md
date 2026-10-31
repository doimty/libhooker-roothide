# Version22 startup audit: later iOS15 dyld and black-screen reports

## Scope and evidence

This branch starts from the actual shipped22 source commit `1c23789d6d6084452d7e3778babf577d816a0539`, package `1.6.9-22+roothide` (DEB SHA256 `5cd0bd27b23bb0a4f3f0af6f00c88738b53059bc645b91f840243b5bb775e79b`). It does not include Configurator/engine23 changes. A friend reported black screen on iOS15.4.1, but their machine ID, jailbreak/basebin build, failure time, tweak list and crash/hang logs have not been supplied. No operation was performed on their device or on the currently functioning connected device.

A source defect is confirmed below. Its occurrence on that particular 15.4.1 environment remains to be confirmed; Apple source tags alone do not establish the exact dyld on either phone.

## Confirmed fork-lifecycle SPI misuse

Shipped22 resolves `_dyld_dlopen_atfork_prepare` and `_dyld_dlopen_atfork_parent`, calls prepare before every NOLOAD query, and calls parent afterward. The comment claims the pair only acquires/releases a normal recursive lock.

In Apple's `dyld-955`:

1. The exported prepare wrapper reaches `RuntimeState::takeDlopenLockBeforeFork()`.
2. It locks apiLock AND sets `stillForking=true`.
3. `APIs::dlopen_from` rejects a non-null path while this flag is set, before normal loading, returning `cannot dlopen until fork() handlers have completed`.
4. The parent wrapper unlocks and clears the flag. Native dlclose is also rejected during the interval.

Thus the shipped wrapper can deterministically reject the very loaded-image query it is meant to protect. This can make `MSGetImageByName(IOSurface)` return NULL and reach Dopamine's first `fix__iosConnect` assertion. It also temporarily exposes unrelated dlopen/dlclose callers to that fork state.

Apple's `dyld-941.5` does not export this named pair in the same glue; it uses static callbacks registered through pthread_atfork. Export absence on an earlier system skips the bad wrapper and is consistent with an earlier-device pass, but no source tag is used here as a substitute for a real device version/trace.

References:

- https://github.com/apple-oss-distributions/dyld/blob/dyld-955/dyld/DyldRuntimeState.cpp#L2314-L2334
- https://github.com/apple-oss-distributions/dyld/blob/dyld-955/dyld/DyldAPIs.cpp#L1222-L1232
- https://github.com/apple-oss-distributions/dyld/blob/dyld-955/dyld/DyldAPIs.cpp#L1429-L1454
- https://github.com/apple-oss-distributions/dyld/blob/dyld-955/libdyld/libdyldGlue.cpp
- https://github.com/apple-oss-distributions/dyld/blob/dyld-941.5/libdyld/libdyldGlue.cpp

## Focused patch and regression

The patch removes the fork-SPI pseudo-lock. It preserves public NOLOAD/LOCAL/FIRST/LAZY flags, exact dyld name lookup before realpath, owned handle lifetime, header validation and paired close. It does not use ordinary dlopen to turn a lookup into a new load, decode private handles, disable assertions, or patch a private dyld mutex.

The image-lookup fixture now advertises the two fork callbacks and models Apple's stillForking rejection. Fixed code must return the pinned target and never look up/call either lifecycle hook. The same test is also compiled against the actual shipped22 source from Git history as a negative control: that source must reject the loaded image at the expected assertion. Existing missing-SPI, shared-cache ENOENT, path-alias, malformed-header and reference-cleanup cases remain.

Candidate package revision `1.6.9-22+roothide.1` is intentionally lower than engine23. It is a standalone startup compatibility patch and does not implement the Configurator preferences protocol. The diagnostic tool only reports whether the fork SPI exports exist; it never calls them.

## Other reviewed startup and recovery risks

These are separate from proving this friend's black-screen cause. They are NOT silently changed in the focused patch.

- The void Substrate/ElleKit Hook wrappers discard error results. In the irrecoverable `LIBHOOKER_ERR_ROLLBACK` case, the transaction layer may retain uncertain code/resources and return after resuming threads. The wrapper currently permits initialization to continue. Ordinary failure does not necessarily mean a NULL-original call, because a failed replacement may never be installed. A separate fault-injection and fatal/recovery-policy design is required before changing this boundary.
- Managers use RTLD_NOW rather than the upstream loader's RTLD_LAZY. A manager can fail before its constructor while ordinary tweaks continue loading. An installed failing manager and its dyld error are needed to attribute this on the friend's device.
- SpringBoard's crash guard and `.eksafemode` do not cover every daemon or earlier RootHide initialization. A pre-SpringBoard failure cannot display SpringBoard recovery UI. The recovery UI also depends on eligible foreground scenes and visible status-bar state; that condition is not evidence that the friend's scene setup actually fails it.
- Crash forwarding uses process-directed kill rather than calling-thread-directed raise, which can reduce fault-thread diagnostic fidelity in multithreaded SpringBoard. It does not by itself prove a hang.
- The indexed dyld name/header lookup race and older native NOLOAD refcount synchronization concern remain separate. An engine-local mutex cannot fix arbitrary external dlclose callers. Removing fork-hook misuse is not a claim to solve every unload race.
- No new shared-cache entry-width or data-PAC bug was established. Do not choose cache layout only by OS number, blanket-sign data storage addresses, or remove RootHide memory-backend requirements.

## Interpreting the injected-library screenshot

Configurator's disable setting gates the plugin scan after RootHide infrastructure and the engine may already be loaded. It cannot unload systemhook, roothideinit, libroothide, forkfix, roothidepatch or libjoker from a running process.

Dopamine's RootHide blacklist is evaluated earlier at process spawn and can select a separate non-injection path. This fork's optional executable whitelist can alter blacklist precedence (`libroothider/src/blacklist.m`); a GUI cannot promise isolation by changing only the Configurator plist. Any integration with RootHide app isolation must be a separate explicit feature with fresh-process verification. No library enumeration is falsified by this patch.

## New evidence: RootHide dyld fix / Spinlock Fix enabled

The user subsequently confirmed that the friend enabled this setting. This changes runtime paths, but does not establish the black-screen cause.

RootHide generates its patched dyld from the device's own `/usr/lib/dyld` (`libjailbreak/src/basebin_gen.m:38-87,145-154`), changes AMFI/UUID and merges its hooks; it does not upgrade dyld to a different OS version. The user flag selects dyld patching versus signing-flags paths (`libroothider/src/common.m:229-263`). On iOS15 arm64e, the spinlock hook remaps executable shared-cache mappings MAP_FIXED | MAP_PRIVATE at the same addresses (`dyldhook/src/spinlock_fix.c:88-101,128-145`). This does not remove the native fork-state semantics, so the confirmed fork-SPI misuse remains relevant when the patch is enabled.

The per-process `dyld_patch_fallback_enabled` state is determined by patched-dyld jbinfo/check-in availability (`systemhook/src/roothider_main.c:604-629`); it is not simply the inverse of the UI setting. The fallback wrappers preserve NOLOAD flags and tail-call native dyld. `DYLD_IN_CACHE=0` does not remove libdyld or IOSurface from the normal system shared cache.

The original Dopamine warning states that full-blacklisted apps on iOS15 arm64e with Spinlock Fix can have their extensions disabled and can still trigger spinlock panics. The explicit prewarm/extension restrictions are at `launchdhook/src/roothider.m:535-568`. Full RootHide blacklist isolation and Configurator/Choicy plugin suppression are therefore different testing states. No setting, package or device reboot was changed during this audit; the friend's first-failure logs are still needed.

## Needed to attribute and validate iOS15.4.1

Collect the first failure's launchd/panic report if present, otherwise SpringBoard/backboardd/watchdog report or a hang sample, plus machine ID and Dopamine/basebin version. A dyld error mentioning fork handlers would directly confirm the source defect path. If SpringBoard remains alive, scene/safety-marker and tweak/manager loading data distinguish a loader problem from missing recovery UI. Do not request another reboot solely to guess a cause when existing reports can establish it.
