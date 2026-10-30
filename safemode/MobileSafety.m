// SpringBoard 崩溃保护和安全模式界面，保持 roothide 的恢复标记兼容。
#import <UIKit/UIKit.h>
#import <roothide.h>
#include "crash-guard.h"
#import "safe-mode-ui.h"
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>
#include <string.h>
#include <mach/mach_time.h>

static char safeModePath[PATH_MAX];
// 按下音量加键时允许主动进入安全模式。
static BOOL LHVolumeUpPressed(void) {
    CFTypeRef (*createClient)(CFAllocatorRef) = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientCreate");
    void (*setMatching)(CFTypeRef, CFDictionaryRef) = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientSetMatching");
    CFArrayRef (*copyServices)(CFTypeRef) = dlsym(RTLD_DEFAULT, "IOHIDEventSystemClientCopyServices");
    CFTypeRef (*createEvent)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, bool, uint32_t) = dlsym(RTLD_DEFAULT, "IOHIDEventCreateKeyboardEvent");
    CFTypeRef (*copyEvent)(CFTypeRef, CFTypeRef, uint32_t, CFTypeRef, uint32_t) = dlsym(RTLD_DEFAULT, "_IOHIDEventSystemClientCopyEventForService");
    CFIndex (*integerValue)(CFTypeRef, uint32_t) = dlsym(RTLD_DEFAULT, "IOHIDEventGetIntegerValue");
    if (!createClient || !setMatching || !copyServices || !createEvent || !copyEvent || !integerValue) return NO;
    CFTypeRef client = createClient(kCFAllocatorDefault);
    if (!client) return NO;
    setMatching(client, (__bridge CFDictionaryRef)@{@"PrimaryUsagePage": @11, @"PrimaryUsage": @1});
    CFArrayRef services = copyServices(client);
    CFTypeRef event = createEvent(kCFAllocatorDefault, mach_absolute_time(), 0x0c, 0xe9, false, 0);
    BOOL pressed = NO;
    if (services && event) {
        for (CFIndex i = 0; i < CFArrayGetCount(services); i++) {
            CFTypeRef state = copyEvent(client, CFArrayGetValueAtIndex(services, i), 3, event, 0);
            if (state) { pressed |= integerValue(state, 0x30002) != 0; CFRelease(state); }
        }
    }
    if (event) CFRelease(event);
    if (services) CFRelease(services);
    CFRelease(client);
    return pressed;
}

// 在任何 SpringBoard 插件初始化前安装崩溃处理器。
__attribute__((constructor)) static void LHInitializeSafety(void) {
    @autoreleasepool {
        if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.springboard"]) return;
        const char *path = jbroot("/var/mobile/.eksafemode");
        if (!path || strlen(path) >= sizeof(safeModePath) || !LHInstallCrashGuard(path)) return;
        strcpy(safeModePath, path);
        if (access(safeModePath, F_OK) != 0 && LHVolumeUpPressed()) {
            LHRequestSafeMode(safeModePath);
        }
        if (LHIsSafeModeRequested() || access(safeModePath, F_OK) == 0) {
            [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidFinishLaunchingNotification
                object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) {
                    LHStartSafeModeUI([NSString stringWithUTF8String:safeModePath], LHGetSafeModePersistenceError());
                }];
        }
    }
}
