// This file is licensed under the BSD-3 Clause License.
#include "config-policy.h"

static CFDictionaryRef as_dictionary(CFTypeRef value) {
    return value && CFGetTypeID(value) == CFDictionaryGetTypeID() ? (CFDictionaryRef)value : NULL;
}

static CFStringRef as_nonempty_string(CFTypeRef value) {
    return value && CFGetTypeID(value) == CFStringGetTypeID() && CFStringGetLength(value) > 0
        ? (CFStringRef)value : NULL;
}

static CFTypeRef field(CFDictionaryRef dictionary, CFStringRef key) {
    return dictionary ? CFDictionaryGetValue(dictionary, key) : NULL;
}

static bool boolean_field(CFDictionaryRef dictionary, CFStringRef key, bool fallback) {
    CFTypeRef value = field(dictionary, key);
    return value && CFGetTypeID(value) == CFBooleanGetTypeID()
        ? CFBooleanGetValue((CFBooleanRef)value) : fallback;
}

static CFDictionaryRef select_record(CFDictionaryRef preferences,
        CFStringRef bundleIdentifier, CFStringRef executablePath) {
    CFDictionaryRef configs = as_dictionary(field(preferences, CFSTR("tweakconfigs")));
    CFDictionaryRef record = NULL;
    if (bundleIdentifier) {
        CFDictionaryRef bundles = as_dictionary(field(configs, CFSTR("bundles")));
        record = as_dictionary(field(bundles, bundleIdentifier));
    }
    if (!record && executablePath) {
        CFDictionaryRef paths = as_dictionary(field(configs, CFSTR("paths")));
        record = as_dictionary(field(paths, executablePath));
    }
    return record ? record : as_dictionary(field(configs, CFSTR("default")));
}

static bool is_web_content(CFStringRef bundleIdentifier, CFStringRef executablePath) {
    if (bundleIdentifier && CFEqual(bundleIdentifier, CFSTR("com.apple.WebKit.WebContent")))
        return true;
    if (!executablePath) return false;
    CFIndex length = CFStringGetLength(executablePath);
    CFRange component = CFRangeMake(0, length), slash;
    if (CFStringFindWithOptions(executablePath, CFSTR("/"), component, kCFCompareBackwards, &slash))
        component = CFRangeMake(slash.location + 1, length - slash.location - 1);
    return CFStringCompareWithOptions(executablePath, CFSTR("WebKit.WebContent"), component, 0)
            == kCFCompareEqualTo ||
        CFStringCompareWithOptions(executablePath, CFSTR("com.apple.WebKit.WebContent"), component, 0)
            == kCFCompareEqualTo;
}

static bool valid_filename(CFTypeRef value) {
    CFStringRef name = as_nonempty_string(value);
    if (!name || CFStringGetLength(name) <= 6 || !CFStringHasSuffix(name, CFSTR(".dylib")))
        return false;
    for (CFIndex index = 0, length = CFStringGetLength(name); index < length; index++) {
        UniChar character = CFStringGetCharacterAtIndex(name, index);
        if (character == 0 || character == '/' || character == '\\') return false;
    }
    return true;
}

static void validate_entry(const void *key, const void *value, void *context) {
    bool *valid = context;
    if (!valid_filename(key) || !value || CFGetTypeID(value) != CFBooleanGetTypeID()) *valid = false;
}

static bool is_allow_list(CFDictionaryRef record) {
    CFTypeRef value = field(record, CFSTR("allowDeny"));
    int mode = 1;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID() &&
            !CFNumberIsFloatType((CFNumberRef)value)) {
        int parsed;
        if (CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &parsed) && (parsed == 0 || parsed == 1))
            mode = parsed;
    }
    return mode == 0;
}

LHConfigPolicy LHConfigPolicyCreate(CFDictionaryRef preferences,
        CFStringRef bundleIdentifier, CFStringRef executablePath) {
    LHConfigPolicy policy = {._processEnabled = true};
    preferences = as_dictionary(preferences);
    bundleIdentifier = as_nonempty_string(bundleIdentifier);
    executablePath = as_nonempty_string(executablePath);
    CFDictionaryRef record = select_record(preferences, bundleIdentifier, executablePath);
    policy._processEnabled = boolean_field(preferences, CFSTR("tweaksEnabled"), true) &&
        (boolean_field(preferences, CFSTR("webProcessTweaks"), true) ||
            !is_web_content(bundleIdentifier, executablePath)) &&
        boolean_field(record, CFSTR("enableTweaks"), true);
    if (!policy._processEnabled || !boolean_field(record, CFSTR("customConfig"), false)) return policy;

    CFDictionaryRef list = as_dictionary(field(record, CFSTR("tweakConfigs")));
    if (!list) return policy;
    bool valid = true;
    CFDictionaryApplyFunction(list, validate_entry, &valid);
    if (!valid) return policy;
    policy._tweakConfigs = (CFDictionaryRef)CFPropertyListCreateDeepCopy(kCFAllocatorDefault,
        list, kCFPropertyListImmutable);
    if (!policy._tweakConfigs) return policy;
    policy._customConfig = true;
    policy._allowList = is_allow_list(record);
    return policy;
}

bool LHConfigPolicyAllowsProcess(const LHConfigPolicy *policy) {
    return !policy || policy->_processEnabled;
}

bool LHConfigPolicyAllowsTweak(const LHConfigPolicy *policy, CFStringRef tweakFilename) {
    if (!LHConfigPolicyAllowsProcess(policy)) return false;
    if (!policy || !policy->_customConfig) return true;
    CFTypeRef state = valid_filename(tweakFilename) ? field(policy->_tweakConfigs, tweakFilename) : NULL;
    bool selected = state && CFGetTypeID(state) == CFBooleanGetTypeID() &&
        CFBooleanGetValue((CFBooleanRef)state);
    return policy->_allowList ? selected : !selected;
}

void LHConfigPolicyRelease(LHConfigPolicy *policy) {
    if (!policy) return;
    if (policy->_tweakConfigs) CFRelease(policy->_tweakConfigs);
    *policy = (LHConfigPolicy){._processEnabled = true};
}
