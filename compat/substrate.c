// Substrate 公共 C 接口，所有 Hook 最终进入同一份 libhooker。
#include "libhooker.h"
#include "libblackjack.h"
#include "funcMacros.h"
#include "../libhooker/dyld/dyldSyms.h"
#include "../libblackjack/objc/objc.h"
#include <dlfcn.h>
#include <objc/runtime.h>
#include <ptrauth.h>

// 仅返回已加载镜像的 Mach-O 头，不把私有句柄暴露给调用者。
LIBHOOKER_EXPORT const void *MSGetImageByName(const char *path) {
    struct libhooker_image *image = LHOpenLoadedImage(path);
    if (!image) return NULL;
    const void *result = image->imageHeader;
    LHCloseImage(image);
    return result;
}

// Mach-O 头没有独立的引用计数。
LIBHOOKER_EXPORT void MSCloseImage(const void *image) { (void)image; }

// 按镜像或全局查找符号，返回可直接调用的 PAC 函数指针。
LIBHOOKER_EXPORT void *MSFindSymbol(const void *header, const char *name) {
    if (!name) return NULL;
    if (!header) {
        void *exported = dlsym(RTLD_DEFAULT, name[0] == '_' ? name + 1 : name);
        if (exported) return exported;
        size_t count = 0;
        char **paths = LHCopyImagePaths(NULL, &count);
        void *result = NULL;
        for (size_t i = 0; i < count; i++) {
            struct libhooker_image *image = LHOpenLoadedImage(paths[i]);
            if (!image) continue;
            LHFindSymbols(image, &name, &result, 1);
            LHCloseImage(image);
            if (result) break;
        }
        free(paths);
        return result;
    }
    void *result = NULL;
    struct libhooker_image *image = LHOpenImageForHeader(header);
    if (!image) return NULL;
    LHFindSymbols(image, &name, &result, 1);
    LHCloseImage(image);
    return result;
}

// 安装 C 函数 Hook 并提供原函数入口。
LIBHOOKER_EXPORT void MSHookFunction(void *target, void *replacement, void **original) {
    struct LHFunctionHook hook = {target, replacement, original, NULL};
    LHHookFunctions(&hook, 1);
}

// 安装 Objective-C 方法 Hook。
LIBHOOKER_EXPORT void MSHookMessageEx(Class cls, SEL selector, IMP replacement, IMP *original) {
    LBHookMessage(cls, selector, (void *)replacement, original);
}

// 单独处理实例类或元类；原调用复用消息 Hook 的继承分派语义。
static void LHHookClassMethods(Class target, Class hook, Class stubs) {
    unsigned count = 0;
    Method *methods = class_copyMethodList(hook, &count);
    for (unsigned i = 0; i < count; i++) {
        SEL selector = method_getName(methods[i]);
        const char *types = method_getTypeEncoding(methods[i]);
        IMP replacement = method_getImplementation(methods[i]);
        Method previous = class_getInstanceMethod(target, selector);
        // Hook 类也可提供新方法；并发添加失败时重新按已有方法处理。
        if (!previous && class_addMethod(target, selector, replacement, types)) continue;
        LHHookMessageWithStubs(target, selector, (void *)replacement, stubs);
    }
    free(methods);
}

// 普通类与元类均安装一遍，使类方法和实例方法遵守相同的原调用规则。
LIBHOOKER_EXPORT void MSHookClassPair(Class target, Class hook, Class stubs) {
    if (!target || !hook) return;
    LHHookClassMethods(target, hook, stubs);
    LHHookClassMethods(object_getClass((id)target), object_getClass((id)hook), stubs ? object_getClass((id)stubs) : Nil);
}

// 将内存补丁交给统一事务接口。
LIBHOOKER_EXPORT void MSHookMemory(void *target, const void *data, size_t size) {
    target = ptrauth_strip(target, ptrauth_key_function_pointer);
    struct LHMemoryPatch patch = {target, data, size, NULL};
    LHPatchMemory(&patch, 1);
}

// 返回实例变量地址，兼容 ElleKit 公开的辅助 C 符号。
LIBHOOKER_EXPORT void *MSHookIvar(id object, const char *name) {
    if (!object || !name) return NULL;
    Ivar ivar = class_getInstanceVariable(object_getClass(object), name);
    return ivar ? (uint8_t *)(void *)object + ivar_getOffset(ivar) : NULL;
}
