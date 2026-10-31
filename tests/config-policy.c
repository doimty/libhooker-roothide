// Host-only policy tests: real CoreFoundation containers, no injector or dlopen.
/*
 * macOS: xcrun --sdk macosx clang -std=c11 -Wall -Wextra -Werror -O1 -g
 *   -fsanitize=address,undefined -fno-omit-frame-pointer -Iinjector
 *   tests/config-policy.c injector/config-policy.c -framework CoreFoundation
 *   -o /tmp/lh-config-policy-test
 * Then run /tmp/lh-config-policy-test (with ASan/UBSan enabled by the build).
 */
#include "../injector/config-policy.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expression) do { \
    checks++; \
    if (!(expression)) { \
        fprintf(stderr, "config-policy:%d: %s\n", __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    CFMutableDictionaryRef preferences;
    CFMutableDictionaryRef configs;
    CFMutableDictionaryRef bundles;
    CFMutableDictionaryRef paths;
    CFMutableDictionaryRef defaults;
} Fixture;

static CFMutableDictionaryRef dictionary(void) {
    CFMutableDictionaryRef result = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CHECK(result != NULL);
    return result;
}

/* The parent owns the returned child; all fixture children are borrowed. */
static CFMutableDictionaryRef child_dictionary(CFMutableDictionaryRef parent, CFStringRef key) {
    CFMutableDictionaryRef child = dictionary();
    CFDictionarySetValue(parent, key, child);
    CFRelease(child);
    return child;
}

static Fixture fixture(void) {
    Fixture value = {.preferences = dictionary()};
    value.configs = child_dictionary(value.preferences, CFSTR("tweakconfigs"));
    value.bundles = child_dictionary(value.configs, CFSTR("bundles"));
    value.paths = child_dictionary(value.configs, CFSTR("paths"));
    value.defaults = child_dictionary(value.configs, CFSTR("default"));
    return value;
}

static void set_int(CFMutableDictionaryRef target, CFStringRef key, int value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CHECK(number != NULL);
    CFDictionarySetValue(target, key, number);
    CFRelease(number);
}

static void expect(CFDictionaryRef preferences, CFStringRef bundle, CFStringRef path,
        bool process, bool alpha, bool beta, bool unknown) {
    LHConfigPolicy policy = LHConfigPolicyCreate(preferences, bundle, path);
    CHECK(LHConfigPolicyAllowsProcess(&policy) == process);
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Alpha.dylib")) == alpha);
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Beta.dylib")) == beta);
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Unknown.dylib")) == unknown);
    LHConfigPolicyRelease(&policy);
}

static CFMutableDictionaryRef custom_list(CFMutableDictionaryRef record, int mode) {
    CFDictionarySetValue(record, CFSTR("customConfig"), kCFBooleanTrue);
    set_int(record, CFSTR("allowDeny"), mode);
    return child_dictionary(record, CFSTR("tweakConfigs"));
}

static void test_original_defaults(void) {
    expect(NULL, NULL, NULL, true, true, true, true);
    expect(NULL, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    expect(NULL, CFSTR("com.apple.WebKit.WebContent"), CFSTR("WebKit.WebContent"), true, true, true, true);
    CFMutableDictionaryRef empty = dictionary();
    expect(empty, CFSTR(""), CFSTR(""), true, true, true, true);
    expect((CFDictionaryRef)CFSTR("not a dictionary"), NULL, NULL, true, true, true, true);
    expect((CFDictionaryRef)kCFBooleanFalse, NULL, NULL, true, true, true, true);
    expect((CFDictionaryRef)kCFNull, NULL, NULL, true, true, true, true);
    CFArrayRef array = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
    CHECK(array != NULL);
    expect((CFDictionaryRef)array, NULL, NULL, true, true, true, true);
    CFRelease(array);
    LHConfigPolicy policy = LHConfigPolicyCreate(empty, NULL, NULL);
    CHECK(LHConfigPolicyAllowsTweak(&policy, NULL));
    CHECK(LHConfigPolicyAllowsTweak(&policy, (CFStringRef)kCFBooleanTrue));
    CHECK(LHConfigPolicyAllowsProcess(NULL));
    CHECK(LHConfigPolicyAllowsTweak(NULL, NULL));
    LHConfigPolicyRelease(&policy);
    LHConfigPolicyRelease(&policy);
    LHConfigPolicyRelease(NULL);
    CHECK(LHConfigPolicyAllowsProcess(&policy));
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Alpha.dylib")));
    CFRelease(empty);
}

static void test_record_precedence(void) {
    Fixture value = fixture();
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
    CFMutableDictionaryRef path = child_dictionary(value.paths, CFSTR("/Applications/App.app/App"));
    CFDictionarySetValue(path, CFSTR("enableTweaks"), kCFBooleanTrue);
    CFMutableDictionaryRef bundle = child_dictionary(value.bundles, CFSTR("com.example.app"));
    CFDictionarySetValue(bundle, CFSTR("enableTweaks"), kCFBooleanFalse);
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), false, false, false, false);
    CFDictionaryRemoveValue(value.bundles, CFSTR("com.example.app"));
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    CFDictionarySetValue(value.bundles, CFSTR("com.example.app"), kCFBooleanFalse);
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    CFDictionarySetValue(value.paths, CFSTR("/Applications/App.app/App"), CFSTR("invalid record"));
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), false, false, false, false);
    expect(value.preferences, CFSTR("unknown.bundle"), CFSTR("/usr/libexec/unknown"), false, false, false, false);
    CFDictionarySetValue(value.configs, CFSTR("default"), kCFNull);
    expect(value.preferences, CFSTR("unknown.bundle"), CFSTR("/usr/libexec/unknown"), true, true, true, true);
    CFRelease(value.preferences);
}

static void test_springboard_and_daemon_paths(void) {
    Fixture value = fixture();
    CFStringRef springboard = CFSTR("/System/Library/CoreServices/SpringBoard.app/SpringBoard");
    CFMutableDictionaryRef springboardRecord = child_dictionary(value.paths, springboard);
    CFDictionarySetValue(springboardRecord, CFSTR("enableTweaks"), kCFBooleanFalse);
    expect(value.preferences, CFSTR("com.apple.springboard"), springboard, false, false, false, false);
    expect(value.preferences, NULL, springboard, false, false, false, false);
    CFMutableDictionaryRef bundle = child_dictionary(value.bundles, CFSTR("com.apple.springboard"));
    CFDictionarySetValue(bundle, CFSTR("enableTweaks"), kCFBooleanTrue);
    expect(value.preferences, CFSTR("com.apple.springboard"), springboard, true, true, true, true);
    CFMutableDictionaryRef daemon = child_dictionary(value.paths, CFSTR("/usr/libexec/exampled"));
    CFDictionarySetValue(daemon, CFSTR("enableTweaks"), kCFBooleanFalse);
    expect(value.preferences, NULL, CFSTR("/usr/libexec/exampled"), false, false, false, false);
    expect(value.preferences, CFSTR("runtime.daemon.bundle"), CFSTR("/usr/libexec/exampled"), false, false, false, false);
    /* Path normalization belongs to the caller: these do not match the logical key. */
    expect(value.preferences, NULL, CFSTR("/rootfs/usr/libexec/exampled"), true, true, true, true);
    expect(value.preferences, NULL, CFSTR("/var/containers/Bundle/.jbroot-TEST/usr/libexec/exampled"), true, true, true, true);
    CFRelease(value.preferences);
}

static void test_whole_record_selection(void) {
    Fixture value = fixture();
    CFMutableDictionaryRef defaultList = custom_list(value.defaults, 0);
    CFDictionarySetValue(defaultList, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
    CFMutableDictionaryRef path = child_dictionary(value.paths, CFSTR("/Applications/App.app/App"));
    CFDictionarySetValue(path, CFSTR("enableTweaks"), kCFBooleanFalse);
    CFMutableDictionaryRef bundle = child_dictionary(value.bundles, CFSTR("com.example.app"));
    /* An empty, selected bundle record does not inherit either disable or list. */
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    CFDictionarySetValue(bundle, CFSTR("customConfig"), kCFBooleanFalse);
    CFMutableDictionaryRef list = child_dictionary(bundle, CFSTR("tweakConfigs"));
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    set_int(bundle, CFSTR("allowDeny"), 0);
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    /* customConfig true uses only the selected list and its own mode default. */
    CFDictionarySetValue(bundle, CFSTR("customConfig"), kCFBooleanTrue);
    CFDictionaryRemoveValue(bundle, CFSTR("allowDeny"));
    CFDictionaryRemoveAllValues(list);
    CFDictionarySetValue(list, CFSTR("Beta.dylib"), kCFBooleanTrue);
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, false, true);
    /* Missing local list is original Filter behavior, never the default list. */
    CFDictionaryRemoveValue(bundle, CFSTR("tweakConfigs"));
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    CFDictionarySetValue(bundle, CFSTR("customConfig"), CFSTR("true"));
    CFDictionarySetValue(bundle, CFSTR("tweakConfigs"), kCFNull);
    expect(value.preferences, CFSTR("com.example.app"), CFSTR("/Applications/App.app/App"), true, true, true, true);
    CFRelease(value.preferences);
}

static void test_global_and_process_disables(void) {
    Fixture value = fixture();
    CFMutableDictionaryRef list = custom_list(value.defaults, 0);
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanTrue);
    CFDictionarySetValue(value.preferences, CFSTR("tweaksEnabled"), kCFBooleanFalse);
    expect(value.preferences, NULL, NULL, false, false, false, false);
    CFDictionarySetValue(value.defaults, CFSTR("tweakConfigs"), CFSTR("malformed list"));
    expect(value.preferences, NULL, NULL, false, false, false, false);
    CFDictionarySetValue(value.preferences, CFSTR("tweakconfigs"), CFSTR("malformed configs"));
    expect(value.preferences, NULL, NULL, false, false, false, false);
    CFRelease(value.preferences);

    value = fixture();
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
    CFDictionarySetValue(value.defaults, CFSTR("customConfig"), kCFBooleanTrue);
    CFTypeRef badLists[] = {kCFNull, kCFBooleanTrue, CFSTR("bad")};
    for (size_t index = 0; index < sizeof(badLists) / sizeof(badLists[0]); index++) {
        CFDictionarySetValue(value.defaults, CFSTR("tweakConfigs"), badLists[index]);
        expect(value.preferences, NULL, NULL, false, false, false, false);
    }
    CFDictionaryRemoveValue(value.defaults, CFSTR("tweakConfigs"));
    expect(value.preferences, NULL, NULL, false, false, false, false);
    CFDictionarySetValue(value.preferences, CFSTR("tweaksEnabled"), kCFBooleanTrue);
    expect(value.preferences, NULL, NULL, false, false, false, false);
    CFRelease(value.preferences);
}

static void test_strict_boolean_types(void) {
    Fixture value = fixture();
    int zero = 0, one = 1;
    CFNumberRef numericFalse = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &zero);
    CFNumberRef numericTrue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    CHECK(numericFalse && numericTrue);
    CFTypeRef malformed[] = {numericFalse, numericTrue, CFSTR("false"), CFSTR("true"), kCFNull};
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(value.preferences, CFSTR("tweaksEnabled"), malformed[index]);
        CFDictionarySetValue(value.preferences, CFSTR("webProcessTweaks"), malformed[index]);
        CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), malformed[index]);
        expect(value.preferences, CFSTR("com.apple.WebKit.WebContent"), NULL, true, true, true, true);
    }
    CFDictionaryRemoveValue(value.preferences, CFSTR("tweaksEnabled"));
    CFDictionaryRemoveValue(value.preferences, CFSTR("webProcessTweaks"));
    CFDictionaryRemoveValue(value.defaults, CFSTR("enableTweaks"));
    CFMutableDictionaryRef list = custom_list(value.defaults, 0);
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(value.defaults, CFSTR("customConfig"), malformed[index]);
        expect(value.preferences, NULL, NULL, true, true, true, true);
    }
    CFRelease(numericFalse);
    CFRelease(numericTrue);
    CFRelease(value.preferences);
}

static void test_allow_deny_modes(void) {
    Fixture value = fixture();
    CFMutableDictionaryRef list = custom_list(value.defaults, 0);
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    CFDictionarySetValue(list, CFSTR("Beta.dylib"), kCFBooleanFalse);
    expect(value.preferences, NULL, NULL, true, true, false, false);
    set_int(value.defaults, CFSTR("allowDeny"), 1);
    expect(value.preferences, NULL, NULL, true, false, true, true);
    CFDictionaryRemoveValue(value.defaults, CFSTR("allowDeny"));
    expect(value.preferences, NULL, NULL, true, false, true, true);
    CFTypeRef malformed[] = {kCFBooleanFalse, kCFBooleanTrue, CFSTR("0"), kCFNull};
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(value.defaults, CFSTR("allowDeny"), malformed[index]);
        expect(value.preferences, NULL, NULL, true, false, true, true);
    }
    int invalidModes[] = {-1, 2, 256};
    for (size_t index = 0; index < sizeof(invalidModes) / sizeof(invalidModes[0]); index++) {
        set_int(value.defaults, CFSTR("allowDeny"), invalidModes[index]);
        expect(value.preferences, NULL, NULL, true, false, true, true);
    }
    double floats[] = {0.0, 0.5, 1.0};
    for (size_t index = 0; index < sizeof(floats) / sizeof(floats[0]); index++) {
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &floats[index]);
        CHECK(number != NULL);
        CFDictionarySetValue(value.defaults, CFSTR("allowDeny"), number);
        expect(value.preferences, NULL, NULL, true, false, true, true);
        CFRelease(number);
    }
    int64_t huge = INT64_MAX;
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &huge);
    CHECK(number != NULL);
    CFDictionarySetValue(value.defaults, CFSTR("allowDeny"), number);
    expect(value.preferences, NULL, NULL, true, false, true, true);
    CFRelease(number);
    set_int(value.defaults, CFSTR("allowDeny"), 0);
    CFDictionarySetValue(list, CFSTR("Unknown.dylib"), kCFBooleanTrue);
    expect(value.preferences, NULL, NULL, true, true, false, true);
    CFDictionaryRemoveAllValues(list);
    expect(value.preferences, NULL, NULL, true, false, false, false);
    set_int(value.defaults, CFSTR("allowDeny"), 1);
    expect(value.preferences, NULL, NULL, true, true, true, true);
    CFRelease(value.preferences);
}

static void test_malformed_lists(void) {
    Fixture value = fixture();
    CFDictionarySetValue(value.defaults, CFSTR("customConfig"), kCFBooleanTrue);
    set_int(value.defaults, CFSTR("allowDeny"), 0);
    expect(value.preferences, NULL, NULL, true, true, true, true);
    CFArrayRef array = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
    CHECK(array != NULL);
    int one = 1;
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
    CHECK(number != NULL);
    CFTypeRef malformed[] = {kCFNull, kCFBooleanFalse, CFSTR("list"), array, number};
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(value.defaults, CFSTR("tweakConfigs"), malformed[index]);
        expect(value.preferences, NULL, NULL, true, true, true, true);
    }
    CFMutableDictionaryRef list = child_dictionary(value.defaults, CFSTR("tweakConfigs"));
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(list, CFSTR("Beta.dylib"), malformed[index]);
        expect(value.preferences, NULL, NULL, true, true, true, true);
        set_int(value.defaults, CFSTR("allowDeny"), 1);
        expect(value.preferences, NULL, NULL, true, true, true, true);
        set_int(value.defaults, CFSTR("allowDeny"), 0);
    }
    CFDictionaryRemoveValue(list, CFSTR("Beta.dylib"));
    CFTypeRef badKeys[] = {kCFBooleanTrue, number, CFSTR(""), CFSTR(".dylib"),
        CFSTR("Alpha"), CFSTR("Alpha.DYLIB"), CFSTR("path/Alpha.dylib"), CFSTR("path\\Alpha.dylib")};
    for (size_t index = 0; index < sizeof(badKeys) / sizeof(badKeys[0]); index++) {
        CFDictionarySetValue(list, badKeys[index], kCFBooleanTrue);
        expect(value.preferences, NULL, NULL, true, true, true, true);
        CFDictionaryRemoveValue(list, badKeys[index]);
    }
    UniChar embeddedNul[] = {'A', 0, '.', 'd', 'y', 'l', 'i', 'b'};
    CFStringRef nulName = CFStringCreateWithCharacters(kCFAllocatorDefault, embeddedNul,
        (CFIndex)(sizeof(embeddedNul) / sizeof(embeddedNul[0])));
    CHECK(nulName != NULL);
    CFDictionarySetValue(list, nulName, kCFBooleanTrue);
    expect(value.preferences, NULL, NULL, true, true, true, true);
    CFRelease(nulName);
    CFRelease(number);
    CFRelease(array);
    CFRelease(value.preferences);
}

static void test_exact_names_and_query_types(void) {
    Fixture value = fixture();
    CFMutableDictionaryRef list = custom_list(value.defaults, 0);
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanTrue);
    CFDictionarySetValue(list, CFSTR("Tweak.With.Dots.dylib"), kCFBooleanTrue);
    LHConfigPolicy policy = LHConfigPolicyCreate(value.preferences, NULL, NULL);
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Alpha.dylib")));
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Tweak.With.Dots.dylib")));
    CFStringRef unselected[] = {CFSTR("alpha.dylib"), CFSTR("ALPHA.dylib"), CFSTR("Alpha.DYLIB"),
        CFSTR("Alpha"), CFSTR("/usr/lib/TweakInject/Alpha.dylib"), CFSTR(""), NULL,
        (CFStringRef)kCFBooleanTrue, (CFStringRef)value.preferences};
    for (size_t index = 0; index < sizeof(unselected) / sizeof(unselected[0]); index++)
        CHECK(!LHConfigPolicyAllowsTweak(&policy, unselected[index]));
    LHConfigPolicyRelease(&policy);
    set_int(value.defaults, CFSTR("allowDeny"), 1);
    policy = LHConfigPolicyCreate(value.preferences, NULL, NULL);
    for (size_t index = 0; index < sizeof(unselected) / sizeof(unselected[0]); index++)
        CHECK(LHConfigPolicyAllowsTweak(&policy, unselected[index]));
    LHConfigPolicyRelease(&policy);
    CFRelease(value.preferences);

    value = fixture();
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
    (void)child_dictionary(value.bundles, CFSTR("Com.Example.App"));
    (void)child_dictionary(value.paths, CFSTR("/usr/libexec/ExampleD"));
    expect(value.preferences, CFSTR("Com.Example.App"), NULL, true, true, true, true);
    expect(value.preferences, CFSTR("com.example.app"), NULL, false, false, false, false);
    expect(value.preferences, NULL, CFSTR("/usr/libexec/ExampleD"), true, true, true, true);
    expect(value.preferences, NULL, CFSTR("/usr/libexec/exampled"), false, false, false, false);
    CFRelease(value.preferences);
}

static void test_exact_web_content_gate(void) {
    Fixture value = fixture();
    CFDictionarySetValue(value.preferences, CFSTR("webProcessTweaks"), kCFBooleanFalse);
    expect(value.preferences, CFSTR("com.apple.WebKit.WebContent"), NULL, false, false, false, false);
    expect(value.preferences, CFSTR("com.apple.WebKit.WebContent"), CFSTR("/usr/libexec/Other"), false, false, false, false);
    CFStringRef blockedPaths[] = {CFSTR("WebKit.WebContent"), CFSTR("com.apple.WebKit.WebContent"),
        CFSTR("/usr/libexec/WebKit.WebContent"), CFSTR("/usr/libexec/com.apple.WebKit.WebContent"),
        CFSTR("/System/Library/Frameworks/WebKit.framework/XPCServices/com.apple.WebKit.WebContent.xpc/com.apple.WebKit.WebContent")};
    for (size_t index = 0; index < sizeof(blockedPaths) / sizeof(blockedPaths[0]); index++)
        expect(value.preferences, CFSTR("unrelated.bundle"), blockedPaths[index], false, false, false, false);
    CFStringRef allowedPaths[] = {CFSTR("/tmp/WebKit.WebContentHelper"), CFSTR("/tmp/com.apple.WebKit.WebContent.extra"),
        CFSTR("/tmp/WebBrowser"), CFSTR("/tmp/WebKit.Networking"), CFSTR("/tmp/WebKit.WebContent/Helper"),
        CFSTR("/tmp/webkit.webcontent"), CFSTR("/tmp/WebKit.WebContent/"), CFSTR("/tmp/WebKit.WebContent.dylib"),
        CFSTR("WebKit.WebContentHelper"), CFSTR(""), NULL};
    for (size_t index = 0; index < sizeof(allowedPaths) / sizeof(allowedPaths[0]); index++)
        expect(value.preferences, CFSTR("com.example.webbrowser"), allowedPaths[index], true, true, true, true);
    CFStringRef allowedBundles[] = {CFSTR("com.apple.WebKit.WebContent.extra"), CFSTR("com.apple.WebKit.Networking"),
        CFSTR("com.apple.webkit.webcontent"), CFSTR("WebKit.WebContent"), NULL};
    for (size_t index = 0; index < sizeof(allowedBundles) / sizeof(allowedBundles[0]); index++)
        expect(value.preferences, allowedBundles[index], NULL, true, true, true, true);
    CFMutableDictionaryRef bundle = child_dictionary(value.bundles, CFSTR("com.apple.WebKit.WebContent"));
    CFDictionarySetValue(bundle, CFSTR("enableTweaks"), kCFBooleanTrue);
    expect(value.preferences, CFSTR("com.apple.WebKit.WebContent"), NULL, false, false, false, false);
    CFDictionarySetValue(value.preferences, CFSTR("webProcessTweaks"), kCFBooleanTrue);
    expect(value.preferences, CFSTR("com.apple.WebKit.WebContent"), NULL, true, true, true, true);
    CFDictionarySetValue(bundle, CFSTR("enableTweaks"), kCFBooleanFalse);
    expect(value.preferences, CFSTR("com.apple.WebKit.WebContent"), NULL, false, false, false, false);
    CFRelease(value.preferences);
}

static void test_malformed_containers_and_identities(void) {
    Fixture value = fixture();
    CFTypeRef malformed[] = {CFSTR("wrong"), kCFBooleanFalse, kCFNull};
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(value.configs, CFSTR("bundles"), malformed[index]);
        CFDictionarySetValue(value.configs, CFSTR("paths"), malformed[index]);
        CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
        expect(value.preferences, CFSTR("com.example.app"), CFSTR("/usr/libexec/exampled"), false, false, false, false);
        CFDictionarySetValue(value.configs, CFSTR("default"), malformed[index]);
        expect(value.preferences, CFSTR("com.example.app"), CFSTR("/usr/libexec/exampled"), true, true, true, true);
        /* Replacing the old child released it; borrow a fresh default record. */
        value.defaults = child_dictionary(value.configs, CFSTR("default"));
    }
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
    expect(value.preferences, (CFStringRef)kCFBooleanTrue, (CFStringRef)value.preferences, false, false, false, false);
    for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); index++) {
        CFDictionarySetValue(value.preferences, CFSTR("tweakconfigs"), malformed[index]);
        expect(value.preferences, (CFStringRef)kCFBooleanTrue, (CFStringRef)value.preferences, true, true, true, true);
    }
    CFRelease(value.preferences);
}

static void test_snapshot_ownership_and_immutability(void) {
    Fixture value = fixture();
    CFMutableDictionaryRef list = custom_list(value.defaults, 0);
    CFMutableStringRef name = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, CFSTR("Alpha.dylib"));
    CHECK(name != NULL);
    CFDictionarySetValue(list, name, kCFBooleanTrue);
    CFDictionarySetValue(list, CFSTR("Beta.dylib"), kCFBooleanFalse);
    CFMutableStringRef bundle = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, CFSTR("com.example.app"));
    CFMutableStringRef path = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, CFSTR("/usr/libexec/exampled"));
    CHECK(bundle && path);
    LHConfigPolicy policy = LHConfigPolicyCreate(value.preferences, bundle, path);
    CFDictionaryRemoveAllValues(list);
    CFStringReplaceAll(name, CFSTR("Changed.dylib"));
    CFDictionarySetValue(list, CFSTR("Alpha.dylib"), kCFBooleanFalse);
    CFDictionarySetValue(list, CFSTR("Beta.dylib"), kCFBooleanTrue);
    CFDictionarySetValue(value.preferences, CFSTR("tweaksEnabled"), kCFBooleanFalse);
    CFDictionarySetValue(value.defaults, CFSTR("enableTweaks"), kCFBooleanFalse);
    CFDictionarySetValue(value.defaults, CFSTR("customConfig"), kCFBooleanFalse);
    set_int(value.defaults, CFSTR("allowDeny"), 1);
    CFStringReplaceAll(bundle, CFSTR("com.apple.WebKit.WebContent"));
    CFStringReplaceAll(path, CFSTR("WebKit.WebContent"));
    CFRelease(name);
    CFRelease(bundle);
    CFRelease(path);
    CFRelease(value.preferences);
    CHECK(LHConfigPolicyAllowsProcess(&policy));
    CHECK(LHConfigPolicyAllowsTweak(&policy, CFSTR("Alpha.dylib")));
    CHECK(!LHConfigPolicyAllowsTweak(&policy, CFSTR("Beta.dylib")));
    CHECK(!LHConfigPolicyAllowsTweak(&policy, CFSTR("Unknown.dylib")));
    CHECK(!LHConfigPolicyAllowsTweak(&policy, CFSTR("Changed.dylib")));
    LHConfigPolicyRelease(&policy);
    LHConfigPolicyRelease(&policy);
}

int main(void) {
    test_original_defaults();
    test_record_precedence();
    test_springboard_and_daemon_paths();
    test_whole_record_selection();
    test_global_and_process_disables();
    test_strict_boolean_types();
    test_allow_deny_modes();
    test_malformed_lists();
    test_exact_names_and_query_types();
    test_exact_web_content_gate();
    test_malformed_containers_and_identities();
    test_snapshot_ownership_and_immutability();
    printf("config-policy: %u checks passed (real CoreFoundation containers)\n", checks);
    return EXIT_SUCCESS;
}
