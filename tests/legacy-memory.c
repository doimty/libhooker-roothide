// 直接执行 legacy-memory.c 的准备/发布逻辑；Mach 与签名接口使用桩，不执行复制的代码。
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libhooker/mem/legacy-memory.h"
#include "../libhooker/mem/writeMem.h"

static unsigned checks, allocations, deallocations, protections, invalidations, writes;
static bool failAllocation, failProtection, overlappingPrepare, readable = true;
static void *pages[4];
static bool released[4];
#define REQUIRE(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr, "legacy-memory failure at %d: %s\n", __LINE__, #condition); abort(); } \
} while (0)

// 只提供不可执行的短字节样本与精确 linker 边界；签名桩返回可调用的测试函数。
const uint8_t LHLegacyWriterStart[8] = {1, 2, 3, 4, 5, 6, 7, 8};
#if __APPLE__
__asm__(".globl _LHLegacyWriterEnd\n.set _LHLegacyWriterEnd, _LHLegacyWriterStart + 8\n");
#else
__asm__(".globl LHLegacyWriterEnd\n.set LHLegacyWriterEnd, LHLegacyWriterStart + 8\n");
#endif

static kern_return_t mockAllocate(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size, int flags) {
    REQUIRE(task == mach_task_self() && size == vm_page_size && flags == VM_FLAGS_ANYWHERE);
    if (failAllocation) return KERN_NO_SPACE;
    REQUIRE(allocations < 4);
    pages[allocations] = malloc(size);
    REQUIRE(pages[allocations]);
    *address = (uintptr_t)pages[allocations++];
    return KERN_SUCCESS;
}

static kern_return_t mockDeallocate(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size) {
    REQUIRE(task == mach_task_self() && size == vm_page_size);
    unsigned index;
    for (index = 0; index < allocations && pages[index] != (void *)(uintptr_t)address; index++) {}
    REQUIRE(index < allocations && !released[index]);
    released[index] = true;
    deallocations++;
    free(pages[index]);
    return KERN_SUCCESS;
}

static void mockInvalidate(void *address, size_t size) {
    REQUIRE(address && size == sizeof(LHLegacyWriterStart));
    invalidations++;
}

static kern_return_t mockProtect(vm_map_t task, mach_vm_address_t address, mach_vm_size_t size,
        boolean_t maximum, vm_prot_t protection) {
    REQUIRE(task == mach_task_self() && size == vm_page_size && !maximum);
    REQUIRE(protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    REQUIRE(!memcmp((void *)(uintptr_t)address, LHLegacyWriterStart, sizeof(LHLegacyWriterStart)));
    protections++;
    if (failProtection) return KERN_FAILURE;
    // 确定性地在外层发布前完成另一候选，直接走到 CAS 败者释放分支。
    if (overlappingPrepare) {
        overlappingPrepare = false;
        REQUIRE(LHPrepareLegacyMemory());
    }
    return KERN_SUCCESS;
}

bool LHGetMemoryProtection(uintptr_t page, uint32_t *protection) {
    if (!readable || !page || (page & (vm_page_size - 1)) || !protection) return false;
    *protection = VM_PROT_READ | VM_PROT_EXECUTE;
    return true;
}

static kern_return_t mockWriter(vm_map_t task, void *destination, const void *source,
        size_t size, vm_prot_t protection) {
    REQUIRE(task == mach_task_self() && destination && source && size == vm_page_size);
    REQUIRE(protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    writes++;
    return KERN_SUCCESS;
}

#undef ptrauth_sign_unauthenticated
#define ptrauth_sign_unauthenticated(value, key, extra) ((void *)(uintptr_t)&mockWriter)
#define mach_vm_allocate mockAllocate
#define mach_vm_deallocate mockDeallocate
#define LHLegacyProtect mockProtect
#define sys_icache_invalidate mockInvalidate
#include "../libhooker/mem/legacy-memory.c"
#undef mach_vm_allocate
#undef mach_vm_deallocate
#undef LHLegacyProtect
#undef sys_icache_invalidate
#undef ptrauth_sign_unauthenticated

int main(int argc, char **argv) {
    const char *mode = argc == 2 ? argv[1] : "ready";
    if (argc > 2) return 2;
    void *destination = (void *)(uintptr_t)0x40000;
    uint8_t *source = malloc(vm_page_size);
    REQUIRE(source);
    REQUIRE(LHLegacyPatch(destination, source, vm_page_size) == KERN_INVALID_ARGUMENT);
    if (!strcmp(mode, "allocation-failure")) {
        failAllocation = true;
        REQUIRE(!LHPrepareLegacyMemory());
        REQUIRE(!allocations && !deallocations && !protections && !invalidations);
        failAllocation = false;
    } else if (!strcmp(mode, "protection-failure")) {
        failProtection = true;
        REQUIRE(!LHPrepareLegacyMemory());
        REQUIRE(allocations == 1 && deallocations == 1 && protections == 1 && invalidations == 1);
        failProtection = false;
    } else if (!strcmp(mode, "overlapping-prepare")) {
        overlappingPrepare = true;
    } else if (strcmp(mode, "ready")) return 2;
    REQUIRE(LHPrepareLegacyMemory());
    unsigned readyAllocations = allocations, readyProtections = protections;
    REQUIRE(LHPrepareLegacyMemory());
    REQUIRE(allocations == readyAllocations && protections == readyProtections);
    REQUIRE(allocations - deallocations == 1);
    REQUIRE(invalidations == protections);
    REQUIRE(LHLegacyPatch(NULL, source, vm_page_size) == KERN_INVALID_ARGUMENT);
    REQUIRE(LHLegacyPatch(destination, NULL, vm_page_size) == KERN_INVALID_ARGUMENT);
    REQUIRE(LHLegacyPatch(destination, source, vm_page_size - 1) == KERN_INVALID_ARGUMENT);
    readable = false;
    REQUIRE(LHLegacyPatch(destination, source, vm_page_size) == KERN_INVALID_ARGUMENT);
    readable = true;
    REQUIRE(!writes);
    REQUIRE(LHLegacyPatch(destination, source, vm_page_size) == KERN_SUCCESS && writes == 1);
    if (!strcmp(mode, "overlapping-prepare")) {
        REQUIRE(allocations == 2 && deallocations == 1);
        REQUIRE(released[0] && !released[1]);  // 外层败者释放，内层已发布页保留。
    }
    printf("PASS legacy memory %s: %u checks, %u allocations, %u released candidates\n",
        mode, checks, allocations, deallocations);
    for (unsigned index = 0; index < allocations; index++) if (!released[index]) free(pages[index]);
    free(source);
    return 0;
}
