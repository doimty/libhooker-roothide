// objc.c — libblackjack
// Created by CoolStar on 2/24/20.
// Copyright © 2020 CoolStar. All rights reserved.
// 沿用上游运行时编码的继承跳板：复用代码模板，每组槽位使用独立参数页。
#include "objc.h"
#include "../include/libblackjack.h"
#include "funcMacros.h"
#include "../../libhooker/as-aarch64/as-aarch64.h"
#include "../../libhooker/mem/writeMem.h"
#include <mach/mach.h>
#include <objc/runtime.h>
#include <ptrauth.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define LH_OBJC_INSTRUCTION_COUNT 24
#define LH_OBJC_TRAMPOLINE_SIZE (LH_OBJC_INSTRUCTION_COUNT * sizeof(uint32_t))

struct LHObjCParameters {
    void *function;
    Class superclass;
    SEL selector;
};

static pthread_mutex_t methodLock = PTHREAD_MUTEX_INITIALIZER;
static void *objcTrampolinePage;
static void *currentTrampolinePage;
static size_t currentTrampolineCount;

// Q 寄存器按十六字节缩放；复用上游寄存器对编码并设置 SIMD 位。
static uint32_t LHVectorPair(bool pop, uint8_t first) {
    return assemble_regp(1, pop, pop ? 16 : -16, first, first + 1) | (1U << 26);
}

// 每个 original 动态查找父类 IMP，完整保留整数、向量、间接返回值和调用者栈。
static void LHEncodeObjCTrampoline(uint32_t *code, size_t parameterOffset) {
    code[0] = assemble_regp(1, 0, -16, 30, 8);
    code[1] = assemble_regp(1, 0, -16, 6, 7);
    code[2] = assemble_regp(1, 0, -16, 4, 5);
    code[3] = assemble_regp(1, 0, -16, 2, 3);
    code[4] = assemble_regp(1, 0, -16, 0, 1);
    code[5] = LHVectorPair(false, 6);
    code[6] = LHVectorPair(false, 4);
    code[7] = LHVectorPair(false, 2);
    code[8] = LHVectorPair(false, 0);
    code[9] = assemble_ldri(1, parameterOffset + offsetof(struct LHObjCParameters, superclass) - 9 * 4, 0);
    code[10] = assemble_ldri(1, parameterOffset + offsetof(struct LHObjCParameters, selector) - 10 * 4, 1);
    code[11] = assemble_ldri(1, parameterOffset + offsetof(struct LHObjCParameters, function) - 11 * 4, 2);
    code[12] = assemble_br(BLR_OP, 2, 0, PAC_AAZ);
    code[13] = 0xaa0003e9; // mov x9, x0
    code[14] = LHVectorPair(true, 0);
    code[15] = LHVectorPair(true, 2);
    code[16] = LHVectorPair(true, 4);
    code[17] = LHVectorPair(true, 6);
    code[18] = assemble_regp(1, 1, 16, 0, 1);
    code[19] = assemble_regp(1, 1, 16, 2, 3);
    code[20] = assemble_regp(1, 1, 16, 4, 5);
    code[21] = assemble_regp(1, 1, 16, 6, 7);
    code[22] = assemble_regp(1, 1, 16, 30, 8);
    code[23] = assemble_br(BR_OP, 9, 0, PAC_AAZ);
}

// 首次运行时编码模板，随后按上游方案映射代码页并保留相邻的可写参数页。
static void *LHGetObjCTrampolinePage(void) {
    size_t pageSize = vm_page_size;
    size_t count = pageSize / LH_OBJC_TRAMPOLINE_SIZE;
    if (!count || sizeof(struct LHObjCParameters) > LH_OBJC_TRAMPOLINE_SIZE ||
            pageSize > 0xffffc) return NULL;
    if (!objcTrampolinePage) {
        uint8_t *code = calloc(1, pageSize);
        if (!code) return NULL;
        for (size_t i = 0; i < count; i++) {
            size_t parameterOffset = pageSize - i * LH_OBJC_TRAMPOLINE_SIZE + i * sizeof(struct LHObjCParameters);
            LHEncodeObjCTrampoline((uint32_t *)(code + i * LH_OBJC_TRAMPOLINE_SIZE), parameterOffset);
        }
        bool success = LHExecMemory(&objcTrampolinePage, code, pageSize);
        free(code);
        if (!success) return NULL;
    }
    vm_address_t allocation = 0;
    if (vm_allocate(mach_task_self(), &allocation, pageSize * 2, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return NULL;
#ifdef LH_ROOTHIDE
    // roothide 的新映射需要独立授权执行，不能只依赖 remap 继承的 RX 权限。
    memcpy((void *)allocation, objcTrampolinePage, pageSize);
    if (!LHMarkMemoryExecutable((void *)allocation)) {
        vm_deallocate(mach_task_self(), allocation, pageSize * 2);
        return NULL;
    }
#else
    vm_address_t codePage = allocation;
    vm_prot_t currentProtection, maximumProtection;
    kern_return_t result = vm_remap(mach_task_self(), &codePage, pageSize, pageSize - 1,
        VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED, mach_task_self(), (vm_address_t)objcTrampolinePage,
        false, &currentProtection, &maximumProtection, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS || codePage != allocation ||
            currentProtection != (VM_PROT_READ | VM_PROT_EXECUTE)) {
        vm_deallocate(mach_task_self(), allocation, pageSize * 2);
        return NULL;
    }
#endif
    return (void *)allocation;
}

// 在方法安装锁内分配上游槽位，填充参数后才发布可调用的认证指针。
static IMP LHInheritedTrampoline(Class superclass, SEL selector) {
    size_t pageSize = vm_page_size;
    if (!currentTrampolinePage || currentTrampolineCount == pageSize / LH_OBJC_TRAMPOLINE_SIZE) {
        void *page = LHGetObjCTrampolinePage();
        if (!page) return NULL;
        currentTrampolinePage = page;
        currentTrampolineCount = 0;
    }
    struct LHObjCParameters *parameters = (void *)((uintptr_t)currentTrampolinePage + pageSize);
    parameters[currentTrampolineCount] = (struct LHObjCParameters){(void *)&class_getMethodImplementation, superclass, selector};
    void *entry = (void *)((uintptr_t)currentTrampolinePage + LH_OBJC_TRAMPOLINE_SIZE * currentTrampolineCount++);
    return ptrauth_sign_unauthenticated(entry, ptrauth_key_function_pointer, 0);
}

// 在同一安装锁内先发布 original 或 ClassPair 存根，最后替换目标方法。
static enum LIBHOOKER_ERR LHInstallMessage(Class cls, SEL selector, void *replacement, void *oldPointer, Class stubs) {
    if (!cls || !selector || !replacement) return LIBHOOKER_ERR_SELECTOR_NOT_FOUND;
    // 首次查询可能执行用户的 +resolveInstanceMethod:，必须在安装锁之外完成。
    if (!class_getInstanceMethod(cls, selector)) return LIBHOOKER_ERR_SELECTOR_NOT_FOUND;
    Class superclass = class_getSuperclass(cls);
    bool hasSuperclassMethod = (oldPointer || stubs) && superclass && class_getInstanceMethod(superclass, selector);
    pthread_mutex_lock(&methodLock);
    Method method = class_getInstanceMethod(cls, selector);
    IMP inherited = NULL;
    if (hasSuperclassMethod && method == class_getInstanceMethod(superclass, selector)) {
        inherited = LHInheritedTrampoline(superclass, selector);
        if (!inherited) { pthread_mutex_unlock(&methodLock); return LIBHOOKER_ERR_VM; }
    }
    // 直接读取 Method 避免提前触发 +initialize，并让子类原调用继续跟随父类后续修改。
    IMP original = inherited ?: method_getImplementation(method);
    if (oldPointer) *(IMP *)oldPointer = original;
    if (stubs) class_replaceMethod(stubs, selector, original, method_getTypeEncoding(method));
    atomic_thread_fence(memory_order_release);
    class_replaceMethod(cls, selector, (IMP)replacement, method_getTypeEncoding(method));
    pthread_mutex_unlock(&methodLock);
    return LIBHOOKER_OK;
}

// Hook 实例方法或元类方法，原调用保留完整 ABI 和继承关系。
LIBHOOKER_EXPORT enum LIBHOOKER_ERR LBHookMessage(Class cls, SEL selector, void *replacement, void *oldPointer) {
    return LHInstallMessage(cls, selector, replacement, oldPointer, Nil);
}

// 为 ClassPair 在目标入口生效之前安装可调用的 original 存根。
enum LIBHOOKER_ERR LHHookMessageWithStubs(Class cls, SEL selector, void *replacement, Class stubs) {
    return LHInstallMessage(cls, selector, replacement, NULL, stubs);
}
