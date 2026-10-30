// 保留常用 ElleKit C 入口，供链接旧 install-name 的插件过渡。
#include "libhooker.h"
#include "funcMacros.h"
#include <mach/mach.h>
#include <ptrauth.h>

// 将 ElleKit 的 C 函数 Hook 入口转发到 libhooker。
LIBHOOKER_EXPORT void EKHookFunction(void *target, void *replacement, void **original) {
    struct LHFunctionHook hook = {target, replacement, original, NULL};
    LHHookFunctions(&hook, 1);
}

// libhooker 始终对安装事务加锁，无需切换全局状态。
LIBHOOKER_EXPORT void EKEnableThreadSafety(int enabled) { (void)enabled; }

// 将原始内存写入转换为带错误码的 libhooker 补丁事务。
static kern_return_t LHCompatibilityPatch(void *target, const void *data, size_t size) {
    struct LHMemoryPatch patch = {ptrauth_strip(target, ptrauth_key_function_pointer), data, size, NULL};
    return LHPatchMemory(&patch, 1) == 1 ? KERN_SUCCESS : KERN_FAILURE;
}
LIBHOOKER_EXPORT kern_return_t (*EKHookMemoryRaw)(void *, const void *, size_t) = LHCompatibilityPatch;
