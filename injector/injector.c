// This file is licensed under the BSD-3 Clause License
// Copyright 2022 © Charlotte Belanger

#include "injector.h"
#include "config-policy.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <dirent.h>
#include <dlfcn.h>
#include <os/log.h>
#include <mach-o/dyld.h>
#if !TARGET_OS_OSX
#include <roothide/roothide.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#if LH_TESTING
// 隔离回归仅扫描测试目录，不读取或加载宿主机插件。
#define TWEAKS_DIRECTORY getenv("LH_TEST_TWEAK_DIRECTORY")
#elif TARGET_OS_OSX
#define TWEAKS_DIRECTORY "/Library/TweakInject/"
#else
#define TWEAKS_DIRECTORY jbroot("/usr/lib/TweakInject/")
#endif
#define MAX_TWEAKMANAGERS 20
#define MAX_TWEAKS 1000
#if LH_TESTING
#define CONFIGURATION_PATH getenv("LH_TEST_CONFIGURATION_PATH")
#elif TARGET_OS_OSX
#define CONFIGURATION_PATH "/var/mobile/Library/Preferences/org.coolstar.libhooker.plist"
#else
#define CONFIGURATION_PATH jbroot("/var/mobile/Library/Preferences/org.coolstar.libhooker.plist")
#endif
#define MAX_CONFIGURATION_BYTES (4 * 1024 * 1024)

// 缺失或损坏的配置保留已有行为；读取一次快照，后续插件不会改写当前筛选结果。
static CFDictionaryRef read_configuration(void) {
    const char *path = CONFIGURATION_PATH;
    if (!path || !*path) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > MAX_CONFIGURATION_BYTES) {
        close(fd);
        return NULL;
    }
    size_t length = (size_t)info.st_size;
    UInt8 *bytes = malloc(length);
    if (!bytes) { close(fd); return NULL; }
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += (size_t)count;
    }
    close(fd);
    CFDataRef data = offset == length ? CFDataCreate(kCFAllocatorDefault, bytes, length) : NULL;
    free(bytes);
    if (!data) return NULL;
    CFPropertyListRef preferences = CFPropertyListCreateWithData(kCFAllocatorDefault, data,
        kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (!preferences) return NULL;
    if (CFGetTypeID(preferences) != CFDictionaryGetTypeID()) { CFRelease(preferences); return NULL; }
    return (CFDictionaryRef)preferences;
}

typedef struct {
    bool isUIProcess;
    bool compareExecutableCaseInsensitive;
    CFStringRef executableName;
    CFStringRef bundleIdentifier;
    CFStringRef logicalPath;
} ProcessContext;

typedef struct {
    char *path;
    bool manager;
} SelectedTweak;

// 只收集带完整动态库后缀的目录项。
static int filter_dylib(const struct dirent *entry) {
    const char *dot = strrchr(entry->d_name, '.');
    return dot && strcmp(dot, ".dylib") == 0;
}

// 路径拼接只分配一次，后续复用缓冲区读取同名 plist。
static char *append_path(const char *directory, const char *name) {
    size_t directoryLength = strlen(directory), nameLength = strlen(name);
    if (directoryLength > SIZE_MAX - nameLength - 1) return NULL;
    char *path = malloc(directoryLength + nameLength + 1);
    if (path) {
        memcpy(path, directory, directoryLength);
        memcpy(path + directoryLength, name, nameLength + 1);
    }
    return path;
}

// 保留完整路径；不能让 dirname 改写后续完整性检查使用的字符串。
static char *copy_executable_path(void) {
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) return strdup(path);
    char *expanded = malloc(size);
    if (expanded && _NSGetExecutablePath(expanded, &size) == 0) return expanded;
    free(expanded);
    return NULL;
}

// 一次建立进程身份，所有插件共享；路径和名称均按 UTF-8 处理。
static ProcessContext process_context(const char *path) {
    ProcessContext context = {0};
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFStringRef identifier = mainBundle ? CFBundleGetIdentifier(mainBundle) : NULL;
    if (identifier) context.bundleIdentifier = CFRetain(identifier);
    const char *logicalPath = path;
#if !TARGET_OS_OSX && !LH_TESTING
    if (path) logicalPath = rootfs(path);
#endif
    if (logicalPath && !strncmp(logicalPath, "/rootfs/", 8)) logicalPath += 7;
    if (logicalPath && logicalPath[0] == '/')
        context.logicalPath = CFStringCreateWithCString(kCFAllocatorDefault, logicalPath, kCFStringEncodingUTF8);
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (slash) {
        size_t parentLength = slash - path;
        context.isUIProcess = (parentLength >= 4 && !memcmp(slash - 4, ".app", 4)) ||
            (parentLength >= 6 && !memcmp(slash - 6, ".appex", 6));
        context.executableName = CFStringCreateWithCString(kCFAllocatorDefault, slash + 1, kCFStringEncodingUTF8);
    }
    if (!context.executableName) {
        CFBundleRef bundle = CFBundleGetMainBundle();
        CFURLRef url = bundle ? CFBundleCopyExecutableURL(bundle) : NULL;
        if (url) {
            context.executableName = CFURLCopyLastPathComponent(url);
            context.compareExecutableCaseInsensitive = true;
            CFRelease(url);
        }
    }
    return context;
}

static void release_process_context(ProcessContext *context) {
    if (context->executableName) CFRelease(context->executableName);
    if (context->bundleIdentifier) CFRelease(context->bundleIdentifier);
    if (context->logicalPath) CFRelease(context->logicalPath);
    *context = (ProcessContext){0};
}

// 同时兼容原版 libhooker 与 Substrate 的版本范围格式，拒绝无效范围。
static bool version_matches(CFTypeRef versions) {
    if (!versions) return true;
    if (CFGetTypeID(versions) != CFArrayGetTypeID()) return false;
    CFIndex count = CFArrayGetCount(versions);
    if (count != 1 && count != 2) return false;
    double bounds[2];
    for (CFIndex i = 0; i < count; i++) {
        CFTypeRef value = CFArrayGetValueAtIndex(versions, i);
        if (CFGetTypeID(value) != CFNumberGetTypeID() ||
            !CFNumberGetValue(value, kCFNumberDoubleType, &bounds[i]) || !isfinite(bounds[i])) return false;
    }
    return kCFCoreFoundationVersionNumber >= bounds[0] &&
        (count == 1 || kCFCoreFoundationVersionNumber < bounds[1]);
}

// 使用直接指针或短栈缓冲区查询类；长 UTF-8 名称才需要临时分配。
static bool class_exists(CFStringRef name) {
    const char *direct = CFStringGetCStringPtr(name, kCFStringEncodingUTF8);
    if (direct) return objc_getClass(direct) != Nil;
    char local[256];
    if (CFStringGetCString(name, local, sizeof(local), kCFStringEncodingUTF8))
        return objc_getClass(local) != Nil;
    CFIndex maximum = CFStringGetMaximumSizeForEncoding(CFStringGetLength(name), kCFStringEncodingUTF8);
    if (maximum < 0 || maximum == LONG_MAX) return false;
    char *expanded = malloc((size_t)maximum + 1);
    if (!expanded) return false;
    bool exists = CFStringGetCString(name, expanded, maximum + 1, kCFStringEncodingUTF8) && objc_getClass(expanded) != Nil;
    free(expanded);
    return exists;
}

// 保持 Bundles、Classes、Executables 的短路 OR 顺序及后台进程限制。
static bool filter_matches(CFDictionaryRef filter, const ProcessContext *context) {
    CFArrayRef bundles = CFDictionaryGetValue(filter, CFSTR("Bundles"));
    if (bundles && CFGetTypeID(bundles) != CFArrayGetTypeID()) return false;
    for (CFIndex i = 0, count = bundles ? CFArrayGetCount(bundles) : 0; i < count; i++) {
        CFStringRef identifier = CFArrayGetValueAtIndex(bundles, i);
        if (CFGetTypeID(identifier) != CFStringGetTypeID()) return false;
        if (!context->isUIProcess &&
            (CFStringHasPrefix(identifier, CFSTR("com.apple.UIKit")) ||
             CFStringHasPrefix(identifier, CFSTR("com.apple.TextInput")) ||
             CFStringHasPrefix(identifier, CFSTR("com.apple.TextEntry")))) continue;
        CFBundleRef bundle = CFBundleGetBundleWithIdentifier(identifier);
        if (bundle && CFBundleIsExecutableLoaded(bundle)) return true;
    }

    CFArrayRef classes = CFDictionaryGetValue(filter, CFSTR("Classes"));
    if (classes && CFGetTypeID(classes) != CFArrayGetTypeID()) return false;
    for (CFIndex i = 0, count = classes ? CFArrayGetCount(classes) : 0; i < count; i++) {
        CFStringRef name = CFArrayGetValueAtIndex(classes, i);
        if (CFGetTypeID(name) != CFStringGetTypeID()) return false;
        if (class_exists(name)) return true;
    }

    CFArrayRef executables = CFDictionaryGetValue(filter, CFSTR("Executables"));
    if (executables && CFGetTypeID(executables) != CFArrayGetTypeID()) return false;
    for (CFIndex i = 0, count = executables ? CFArrayGetCount(executables) : 0; i < count; i++) {
        CFStringRef name = CFArrayGetValueAtIndex(executables, i);
        if (CFGetTypeID(name) != CFStringGetTypeID()) return false;
        if (context->executableName && CFStringCompare(context->executableName, name,
            context->compareExecutableCaseInsensitive ? kCFCompareCaseInsensitive : 0) == kCFCompareEqualTo) return true;
    }
    return false;
}

// 解析一个插件的过滤条件；统一释放属性列表，避免失败分支泄漏。
static bool tweak_needinject(const char *path, const ProcessContext *context, bool *isTweakManager) {
    *isTweakManager = false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path, strlen(path), false);
    if (!url) return false;
    CFDataRef data = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    bool read = CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, url, &data, NULL, NULL, NULL);
#pragma clang diagnostic pop
    CFRelease(url);
    if (!read || !data) {
        if (data) CFRelease(data);
        return false;
    }
    CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    if (!plist) return false;
    bool matches = false;
    if (CFGetTypeID(plist) != CFDictionaryGetTypeID()) goto done;
    CFDictionaryRef filter = CFDictionaryGetValue(plist, CFSTR("Filter"));
    if (!filter || CFGetTypeID(filter) != CFDictionaryGetTypeID()) goto done;
    CFBooleanRef manager = CFDictionaryGetValue(plist, CFSTR("IsTweakManager"));
    if (manager && CFGetTypeID(manager) != CFBooleanGetTypeID()) goto done;
    if (!version_matches(CFDictionaryGetValue(filter, CFSTR("CoreFoundationVersion"))) ||
        !version_matches(CFDictionaryGetValue(plist, CFSTR("CoreFoundationVersion")))) goto done;
    matches = filter_matches(filter, context);
    if (matches && manager) *isTweakManager = CFBooleanGetValue(manager);
done:
    CFRelease(plist);
    return matches;
}

// 保持原有反向排序与逆序遍历的组合，不改变插件初始化顺序。
static int alphasort2(const struct dirent **a, const struct dirent **b) {
    return strcoll((*b)->d_name, (*a)->d_name);
}

// 先确定全部过滤结果，再加载管理器和插件，防止前一个插件改变后续筛选。
static void tweaks_iterate(const ProcessContext *context) {
    CFDictionaryRef configuration = read_configuration();
    LHConfigPolicy policy = LHConfigPolicyCreate(configuration, context->bundleIdentifier, context->logicalPath);
    if (configuration) CFRelease(configuration);
    if (!LHConfigPolicyAllowsProcess(&policy)) {
        LHConfigPolicyRelease(&policy);
        return;
    }
    const char *directory = TWEAKS_DIRECTORY;
    if (!directory) { LHConfigPolicyRelease(&policy); return; }
    struct dirent **files = NULL;
    int count = scandir(directory, &files, filter_dylib, alphasort2);
    if (count < 0) { perror("scandir"); LHConfigPolicyRelease(&policy); return; }
    size_t capacity = (size_t)count < MAX_TWEAKMANAGERS + MAX_TWEAKS ? (size_t)count : MAX_TWEAKMANAGERS + MAX_TWEAKS;
    SelectedTweak *selected = capacity ? calloc(capacity, sizeof(*selected)) : NULL;
    int selectedCount = 0, managerCount = 0, tweakCount = 0;
    for (int n = count; n-- > 0;) {
        if (selected) {
            char *path = append_path(directory, files[n]->d_name);
            if (path) {
                size_t length = strlen(path);
                memcpy(path + length - 6, ".plist", 7);
                bool manager = false;
                bool matches = tweak_needinject(path, context, &manager);
                if (matches && !manager) {
                    CFStringRef filename = CFStringCreateWithCString(kCFAllocatorDefault, files[n]->d_name, kCFStringEncodingUTF8);
                    matches = filename && LHConfigPolicyAllowsTweak(&policy, filename);
                    if (filename) CFRelease(filename);
                }
                memcpy(path + length - 6, ".dylib", 7);
                if (matches && (manager ? managerCount < MAX_TWEAKMANAGERS : tweakCount < MAX_TWEAKS)) {
                    selected[selectedCount++] = (SelectedTweak){path, manager};
                    if (manager) managerCount++; else tweakCount++;
                } else free(path);
            }
        }
        free(files[n]);
    }
    free(files);

    if (tweakCount) {
        for (int i = 0; i < selectedCount; i++) {
            if (selected[i].manager && !dlopen(selected[i].path, RTLD_NOW))
                os_log_error(OS_LOG_DEFAULT, "libhooker loader: %{public}s: %{public}s", selected[i].path, dlerror());
        }
#if !TARGET_OS_OSX
        const char *oldABI = jbroot("/usr/lib/ellekit/OldABI.dylib");
        if (oldABI && access(oldABI, F_OK) == 0) dlopen(oldABI, RTLD_LAZY);
#endif
        for (int i = 0; i < selectedCount; i++) {
            if (!selected[i].manager && !dlopen(selected[i].path, RTLD_NOW))
                os_log_error(OS_LOG_DEFAULT, "libhooker loader: %{public}s: %{public}s", selected[i].path, dlerror());
        }
    }
    for (int i = 0; i < selectedCount; i++) free(selected[i].path);
    free(selected);
    LHConfigPolicyRelease(&policy);
}

#if !TARGET_OS_OSX || LH_LOADER_UNIT_TEST
// 所有保护检查都在扫描和 dlopen 之前完成；目录异常只停用插件，不终止宿主。
static void inject_process_tweaks(const ProcessContext *context, const char *path,
    const char *safeModeMarker, const char *compatibilityDirectory, bool safeModeRequested) {
    if (safeModeRequested || !path || !*path) return;
    if (context->isUIProcess && safeModeMarker && access(safeModeMarker, F_OK) == 0) return;
    if (path && strstr(path, ".app/")) {
        struct stat st = {0};
        if (!compatibilityDirectory || lstat(compatibilityDirectory, &st) != 0 || !S_ISLNK(st.st_mode)) {
            os_log_error(OS_LOG_DEFAULT, "libhooker loader files are incomplete; skipping plugin injection");
            return;
        }
    }
    tweaks_iterate(context);
}
#endif

#if !LH_LOADER_UNIT_TEST
// 处理安全模式、保护准备和路径校验，再启动当前进程的插件加载。
__attribute__((constructor)) static void injection_init(void) {
    const char *extension = getenv("SANDBOX_EXTENSION");
    if (extension) sandbox_extension_consume(extension);
    const char *msSafe = getenv("_MSSafeMode"), *safe = getenv("_SafeMode");
    if ((msSafe && atoi(msSafe) == 1) || (safe && atoi(safe) == 1)) return;

#if !TARGET_OS_OSX
    bool safeModeRequested = false;
    CFBundleRef bundle = CFBundleGetMainBundle();
    CFStringRef identifier = bundle ? CFBundleGetIdentifier(bundle) : NULL;
    if (identifier && CFEqual(identifier, CFSTR("com.apple.springboard"))) {
        void *safety = dlopen(jbroot("/usr/lib/libhooker/MobileSafety.dylib"), RTLD_NOW);
        bool (*isReady)(void) = safety ? dlsym(safety, "LHCrashGuardIsReady") : NULL;
        bool (*isRequested)(void) = safety ? dlsym(safety, "LHIsSafeModeRequested") : NULL;
        // 动态库加载成功不代表信号处理器注册成功，保护未就绪时停止注入。
        if (!isReady || !isReady() || !isRequested) {
            os_log_error(OS_LOG_DEFAULT, "libhooker safe mode unavailable or not initialized");
            return;
        }
        safeModeRequested = isRequested();
    }
#endif
    char *path = copy_executable_path();
    if (!path || !*path) {
        free(path);
        os_log_error(OS_LOG_DEFAULT, "libhooker executable path unavailable; skipping plugin injection");
        return;
    }
    ProcessContext context = process_context(path);
#if !TARGET_OS_OSX
    inject_process_tweaks(&context, path, jbroot("/var/mobile/.eksafemode"),
        jbroot("/Library/MobileSubstrate/DynamicLibraries"), safeModeRequested);
#else
    tweaks_iterate(&context);
#endif
    release_process_context(&context);
    free(path);
}
#endif
