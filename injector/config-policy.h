// This file is licensed under the BSD-3 Clause License.
#ifndef LH_CONFIG_POLICY_H
#define LH_CONFIG_POLICY_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configurator compatibility contract for this loader.
 *
 * This is a newly defined reader contract based on the sibling Configurator's
 * ConfigViewController serialization, not a claim about the unavailable original
 * native reader. No preferences, paths, plugins, or process state are read here.
 *
 * Preferences (all keys and identities are exact and case sensitive):
 *   tweaksEnabled: CFBoolean, default true
 *   webProcessTweaks: CFBoolean, default true
 *   tweakconfigs: {
 *     default: record,
 *     bundles: { bundleIdentifier: record },
 *     paths: { logicalExecutablePath: record }
 *   }
 * A record is selected once: matching dictionary under bundles, then matching
 * dictionary under paths, then dictionary under default, then built-in defaults.
 * Missing or non-dictionary candidates are skipped. An empty dictionary IS a
 * complete selected record. There is no field inheritance from other records.
 * The caller supplies an already normalized logical path (including any jbroot
 * or /rootfs removal); this module never normalizes or folds case in a path.
 *
 * Selected record fields:
 *   enableTweaks: CFBoolean, default true
 *   customConfig: CFBoolean, default false (original Filter behavior)
 *   allowDeny: integer CFNumber 0 = Allow, 1 = Deny; default 1
 *   tweakConfigs: { "Filename.dylib": CFBoolean, ... }
 * The UI inserts [Deny, Allow] at segment index zero, reversing their order.
 * Only customConfig true activates tweakConfigs. Allow permits selected true
 * filenames only; Deny blocks selected true filenames only. An absent filename
 * is unselected, as is a false value. An empty Allow dictionary permits none.
 *
 * Booleans and numbers are never coerced into each other. Malformed fields use
 * their field defaults. A missing or malformed tweakConfigs dictionary disables
 * only the per-tweak restriction; valid global/process disables still apply.
 * List keys must be nonempty basenames ending exactly in .dylib, with a nonempty
 * stem, no slash, backslash, or embedded NUL, and values must be CFBoolean. One
 * invalid entry makes the entire list malformed. Unknown valid filenames are
 * unselected. An invalid query name is also unselected when a list is active.
 *
 * tweaksEnabled false always disables the process. webProcessTweaks false also
 * disables the exact bundle com.apple.WebKit.WebContent or the exact executable
 * basename WebKit.WebContent / com.apple.WebKit.WebContent (including a full path
 * ending in that component). Other WebKit or web-like names are unaffected.
 * A NULL or non-dictionary preference object permits the original defaults.
 *
 * Integration: process disable and safe mode must gate ALL plugins. After the
 * loader's existing Filter matches, apply AllowsTweak to ordinary tweaks only;
 * IsTweakManager may bypass the per-tweak restriction, never the process gate.
 */

/*
 * Treat these implementation fields as read-only. Create returns one owning
 * value containing an immutable deep copy of the validated filename dictionary.
 * All preference and identity arguments are borrowed only during Create; they
 * can be mutated or released afterward. Reuse the snapshot for every tweak.
 * Do not copy an owning snapshot and release both copies. Release exactly once
 * when finished (repeated Release is harmless and resets to original defaults).
 * Allocation failure drops the per-tweak restriction, preserving process gates.
 * Immutable snapshots may be queried concurrently while they remain alive.
 */
typedef struct {
    bool _processEnabled;
    bool _customConfig;
    bool _allowList;
    CFDictionaryRef _tweakConfigs;
} LHConfigPolicy;

LHConfigPolicy LHConfigPolicyCreate(CFDictionaryRef preferences,
    CFStringRef bundleIdentifier, CFStringRef executablePath);

/* A NULL snapshot means original defaults, as does Create(NULL, NULL, NULL). */
bool LHConfigPolicyAllowsProcess(const LHConfigPolicy *policy);
bool LHConfigPolicyAllowsTweak(const LHConfigPolicy *policy, CFStringRef tweakFilename);
void LHConfigPolicyRelease(LHConfigPolicy *policy);

#ifdef __cplusplus
}
#endif

#endif
