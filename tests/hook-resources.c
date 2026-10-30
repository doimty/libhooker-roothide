// 用隔离输出槽和内核接口桩验证真实 Hook 资源事务，不改写系统内存。
#include "../libhooker/mem/hook-transaction.h"
#include "../libhooker/mem/header-relay.h"
#include "../libhooker/mem/shadowMem.h"
#include "../libhooker/mem/threadSafety.h"
#include "../libhooker/mem/writeMem.h"
#include <mach/arm/thread_status.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAX_RESOURCES 16
static unsigned checks;
#define REQUIRE(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "hook-resources failure at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

static uint8_t *slots;
static struct {
    struct LHHookResources *records[MAX_RESOURCES];
    void *mappings[MAX_RESOURCES * 2];
    unsigned recordCount, mappingCount, recordFrees, unmaps;
    unsigned reads, failRead, shortRead, corruptRead, protections, dirtyQueries;
    uintptr_t dirtyStart, dirtyEnd;
    uint32_t protection[2];
    bool protectionFails, headerValid, headerPreserved;
    unsigned headerFinishes, stateReads;
    uintptr_t pc[4], lr[4];
    kern_return_t stateResult[4];
    mach_msg_type_number_t stateCount;
} mock;

// 声明生产资源实现所使用的隔离释放与读取入口。
static void mockFreeRecord(void *record);
static int mockUnmap(void *address, size_t size);
static kern_return_t mockReadPointer(vm_map_read_t task, mach_vm_address_t address,
    mach_vm_size_t size, mach_vm_address_t output, mach_vm_size_t *copied);
static kern_return_t mockThreadState(thread_act_t thread, thread_state_flavor_t flavor,
    thread_state_t output, mach_msg_type_number_t *count);

// 仅替换外部依赖，实际队列、校验、发布、恢复与资源选择逻辑均来自生产源码。
#define free mockFreeRecord
#define munmap mockUnmap
#define mach_vm_read_overwrite mockReadPointer
#define thread_get_state mockThreadState
#include "../libhooker/mem/hook-transaction.c"
#undef thread_get_state
#undef mach_vm_read_overwrite
#undef munmap
#undef free

// 生产实现只能释放本轮登记的队列节点，拒绝重复释放。
static void mockFreeRecord(void *record) {
    for (unsigned i = 0; i < mock.recordCount; i++) {
        if (mock.records[i] != record) continue;
        mock.records[i] = NULL;
        mock.recordFrees++;
        free(record);
        return;
    }
    REQUIRE(false);
}

// trampoline 和 relay 使用普通隔离缓冲区，仅模拟整页释放。
static int mockUnmap(void *address, size_t size) {
    REQUIRE(size == PAGE_SIZE);
    for (unsigned i = 0; i < mock.mappingCount; i++) {
        if (mock.mappings[i] != address) continue;
        mock.mappings[i] = NULL;
        mock.unmaps++;
        free(address);
        return 0;
    }
    REQUIRE(false);
    return -1;
}

// 指针读取仅接受隔离槽区，支持失败、短读和恢复后读回不一致。
static kern_return_t mockReadPointer(vm_map_read_t task, mach_vm_address_t address,
        mach_vm_size_t size, mach_vm_address_t output, mach_vm_size_t *copied) {
    REQUIRE(task == mach_task_self() && size == sizeof(void *));
    REQUIRE(address >= (uintptr_t)slots && address - (uintptr_t)slots <= PAGE_SIZE * 2 - size);
    mock.reads++;
    *copied = 0;
    if (mock.reads == mock.failRead) return KERN_FAILURE;
    *copied = mock.reads == mock.shortRead ? size - 1 : size;
    memcpy((void *)output, (void *)address, *copied);
    if (mock.reads == mock.corruptRead) ((uint8_t *)output)[0] ^= 1;
    return KERN_SUCCESS;
}

// 返回只读的线程状态，不真正读取或修改宿主线程寄存器。
static kern_return_t mockThreadState(thread_act_t thread, thread_state_flavor_t flavor,
        thread_state_t output, mach_msg_type_number_t *count) {
    REQUIRE(thread > 0 && thread < 4 && flavor == ARM_THREAD_STATE64);
    REQUIRE(*count == ARM_THREAD_STATE64_COUNT);
    mock.stateReads++;
    if (mock.stateResult[thread] != KERN_SUCCESS) return mock.stateResult[thread];
    arm_thread_state64_t state = {0};
    arm_thread_state64_set_pc_fptr(state, (void (*)(void))mock.pc[thread]);
    arm_thread_state64_set_lr_fptr(state, (void (*)(void))mock.lr[thread]);
    memcpy(output, &state, sizeof(state));
    *count = mock.stateCount;
    return KERN_SUCCESS;
}

// 使用独立半开区间模型，验证输出槽查询的精确重叠边界。
bool LHShadowRangeIsDirty(uintptr_t address, size_t size) {
    mock.dirtyQueries++;
    REQUIRE(size == sizeof(void *));
    return mock.dirtyStart < mock.dirtyEnd && address < mock.dirtyEnd && mock.dirtyStart < address + size;
}

// 返回指定槽页的真实测试权限，不通过任何内核接口修改权限。
bool LHGetMemoryProtection(uintptr_t page, uint32_t *protection) {
    mock.protections++;
    if (mock.protectionFails) return false;
    REQUIRE(page == (uintptr_t)slots || page == (uintptr_t)slots + PAGE_SIZE);
    *protection = mock.protection[(page - (uintptr_t)slots) / PAGE_SIZE];
    return true;
}

// header relay 的复核结果独立注入，避免借资源测试改写镜像头。
bool LHValidateHeaderRelays(void) { return mock.headerValid; }

// 记录最终的保留决定，确认普通 relay 与 header relay 使用同一事务结论。
void LHFinishHeaderRelays(bool preserve) {
    mock.headerFinishes++;
    mock.headerPreserved = preserve;
}

// 为测试创建可追踪的页缓冲区，最终仅由桩或测试自身释放。
static void *newMapping(void) {
    REQUIRE(mock.mappingCount < MAX_RESOURCES * 2);
    void *address = malloc(PAGE_SIZE);
    REQUIRE(address != NULL);
    mock.mappings[mock.mappingCount++] = address;
    return address;
}

// 每项入队携带独立 trampoline 和 relay，以便检查资源保留决定。
static struct LHHookResources *queueResource(void **oldptr, uintptr_t original, size_t patchSize) {
    REQUIRE(mock.recordCount < MAX_RESOURCES);
    struct LHHookResources *entry = calloc(1, sizeof(*entry));
    REQUIRE(entry != NULL);
    entry->oldptr = oldptr;
    entry->original = (void *)original;
    entry->trampoline = newMapping();
    entry->relay = newMapping();
    entry->target = 0x1000;
    entry->patchSize = patchSize;
    mock.records[mock.recordCount++] = entry;
    LHQueueHookResources(entry);
    return entry;
}

// 上轮已经完成公共收尾；测试自行清掉故意保留的假映射，避免工具报告假泄漏。
static void resetFixture(void) {
    for (unsigned i = 0; i < mock.recordCount; i++) REQUIRE(mock.records[i] == NULL);
    for (unsigned i = 0; i < mock.mappingCount; i++) free(mock.mappings[i]);
    memset(&mock, 0, sizeof(mock));
    memset(slots, 0, PAGE_SIZE * 2);
    mock.protection[0] = mock.protection[1] = VM_PROT_READ | VM_PROT_WRITE;
    mock.headerValid = true;
    mock.stateCount = ARM_THREAD_STATE64_COUNT;
}

// 完成事务后检查节点总会释放，而可执行资源服从是否仍可能可达的决定。
static void finishAndRequire(bool preserve) {
    LHFinishHookResources(preserve);
    REQUIRE(mock.recordFrees == mock.recordCount);
    REQUIRE(mock.unmaps == (preserve ? 0 : mock.mappingCount));
    REQUIRE(mock.headerFinishes == 1 && mock.headerPreserved == preserve);
    LHFinishHookResources(false);
    REQUIRE(mock.recordFrees == mock.recordCount);
    REQUIRE(mock.headerFinishes == 2);
}

// oldptr 在准备时才快照，回滚恢复排队后、发布前的真实值。
static void testPointerSnapshotAndRestore(void) {
    resetFixture();
    void **oldptr = (void **)(slots + 32);
    *oldptr = (void *)0x11110000;
    struct LHHookResources *entry = queueResource(oldptr, 0x33330000, 12);
    *oldptr = (void *)0x22220000;
    REQUIRE(LHPrepareHookPointers());
    REQUIRE(entry->previousOriginal == (void *)0x22220000 && *oldptr == (void *)0x22220000);
    REQUIRE(mock.reads == 1 && !mock.unmaps && !mock.recordFrees);
    LHPublishHookPointers();
    REQUIRE(*oldptr == (void *)0x33330000);
    REQUIRE(LHRestoreHookPointers());
    REQUIRE(*oldptr == (void *)0x22220000 && mock.reads == 2);
    finishAndRequire(false);
}

// 同一输出槽重复入队保持发布顺序，所有快照均为事务开始前的同一值。
static void testRepeatedOutputSlot(void) {
    resetFixture();
    void **oldptr = (void **)(slots + 32);
    *oldptr = (void *)0x11110000;
    struct LHHookResources *first = queueResource(oldptr, 0x22220000, 12);
    struct LHHookResources *second = queueResource(oldptr, 0x33330000, 12);
    REQUIRE(LHPrepareHookPointers());
    REQUIRE(first->previousOriginal == (void *)0x11110000 && second->previousOriginal == first->previousOriginal);
    LHPublishHookPointers();
    REQUIRE(*oldptr == (void *)0x33330000);
    REQUIRE(LHRestoreHookPointers() && *oldptr == (void *)0x11110000);
    finishAndRequire(false);
}

// 成功提交保留发布的原调用及全部可执行资源；空输出槽不产生无效访问。
static void testSuccessfulResourceRetention(void) {
    resetFixture();
    void **oldptr = (void **)(slots + 32);
    *oldptr = (void *)0x11110000;
    queueResource(oldptr, 0x22220000, 12);
    queueResource(NULL, 0x33330000, 4);
    REQUIRE(LHPrepareHookPointers() && mock.reads == 1);
    LHPublishHookPointers();
    finishAndRequire(true);
    REQUIRE(*oldptr == (void *)0x22220000);
}

// 不可写、查询失败、短读和读取失败都必须在发布前拒绝并保留原槽值。
static void testPreparationFailures(void) {
    for (unsigned failure = 0; failure < 4; failure++) {
        resetFixture();
        void **oldptr = (void **)(slots + 32);
        *oldptr = (void *)0x11110000;
        queueResource(oldptr, 0x22220000, 12);
        if (failure == 0) mock.protection[0] = VM_PROT_READ;
        if (failure == 1) mock.protectionFails = true;
        if (failure == 2) mock.shortRead = 1;
        if (failure == 3) mock.failRead = 1;
        REQUIRE(!LHPrepareHookPointers());
        REQUIRE(*oldptr == (void *)0x11110000);
        finishAndRequire(false);
    }
}

// 非对齐输出槽会形成部分交叠，必须拒绝而不是按指针直接写入。
static void testPartiallyOverlappingSlots(void) {
    resetFixture();
    void **oldptr = (void **)(slots + 32);
    *oldptr = (void *)0x11110000;
    queueResource(oldptr, 0x22220000, 12);
    queueResource((void **)(slots + 36), 0x33330000, 12);
    REQUIRE(!LHPrepareHookPointers());
    REQUIRE(*oldptr == (void *)0x11110000 && mock.reads == 1);
    finishAndRequire(false);
}

// 输出槽与脏字节仅相邻可以共页，任意一个字节交叠均在发布前拒绝。
static void testDirtyRangeBoundaries(void) {
    const struct { size_t offset, length; bool allowed; } cases[] = {
        {0, 32, true}, {40, 8, true}, {0, 33, false}, {39, 8, false}, {32, 8, false},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        resetFixture();
        void **oldptr = (void **)(slots + 32);
        *oldptr = (void *)0x11110000;
        queueResource(oldptr, 0x22220000, 12);
        mock.dirtyStart = (uintptr_t)slots + cases[i].offset;
        mock.dirtyEnd = mock.dirtyStart + cases[i].length;
        REQUIRE(LHPrepareHookPointers() == cases[i].allowed);
        REQUIRE(mock.dirtyQueries == 1 && *oldptr == (void *)0x11110000);
        if (cases[i].allowed) {
            LHPublishHookPointers();
            REQUIRE(LHRestoreHookPointers() && *oldptr == (void *)0x11110000);
        }
        finishAndRequire(false);
    }
}

// 恢复权限不满足、读回失败或不一致都报告失败，并保留可能仍被引用的资源。
static void testRestoreFailuresRetainResources(void) {
    for (unsigned failure = 0; failure < 5; failure++) {
        resetFixture();
        void **oldptr = (void **)(slots + 32);
        *oldptr = (void *)0x11110000;
        queueResource(oldptr, 0x22220000, 12);
        REQUIRE(LHPrepareHookPointers());
        LHPublishHookPointers();
        if (failure == 0) mock.protection[0] = VM_PROT_READ;
        if (failure == 1) mock.protectionFails = true;
        if (failure == 2) mock.failRead = 2;
        if (failure == 3) mock.shortRead = 2;
        if (failure == 4) mock.corruptRead = 2;
        REQUIRE(!LHRestoreHookPointers());
        REQUIRE(*oldptr == (void *)(uintptr_t)(failure < 2 ? 0x22220000 : 0x11110000));
        finishAndRequire(true);
    }
}

// 首槽失败不能阻止后续槽恢复，也不能让后续成功抹去本次恢复失败。
static void testRestoreContinuesAfterFailure(void) {
    for (unsigned failure = 0; failure < 4; failure++) {
        resetFixture();
        void **first = (void **)(slots + 32);
        void **second = (void **)(slots + PAGE_SIZE + 32);
        *first = (void *)0x11110000;
        *second = (void *)0x44440000;
        queueResource(first, 0x22220000, 12);
        queueResource(second, 0x55550000, 12);
        REQUIRE(LHPrepareHookPointers() && mock.reads == 2);
        LHPublishHookPointers();
        REQUIRE(*first == (void *)0x22220000 && *second == (void *)0x55550000);
        if (failure == 0) mock.protection[0] = VM_PROT_READ;
        if (failure == 1) mock.failRead = 3;
        if (failure == 2) mock.shortRead = 3;
        if (failure == 3) mock.corruptRead = 3;
        REQUIRE(!LHRestoreHookPointers());
        REQUIRE(*first == (void *)(uintptr_t)(failure == 0 ? 0x22220000 : 0x11110000));
        REQUIRE(*second == (void *)0x44440000);
        REQUIRE(mock.reads == (failure == 0 ? 3 : 4) && mock.protections == 4);
        finishAndRequire(true);
    }
}

// 重复槽后一次恢复成功也必须保留前一次读回失败，不能提前释放任一关联资源。
static void testRepeatedSlotRestoreFailure(void) {
    for (unsigned failure = 0; failure < 3; failure++) {
        resetFixture();
        void **oldptr = (void **)(slots + 32);
        *oldptr = (void *)0x11110000;
        struct LHHookResources *first = queueResource(oldptr, 0x22220000, 12);
        struct LHHookResources *second = queueResource(oldptr, 0x33330000, 12);
        REQUIRE(LHPrepareHookPointers() && mock.reads == 2);
        REQUIRE(first->previousOriginal == (void *)0x11110000 && second->previousOriginal == first->previousOriginal);
        LHPublishHookPointers();
        REQUIRE(*oldptr == (void *)0x33330000);
        if (failure == 0) mock.failRead = 3;
        if (failure == 1) mock.shortRead = 3;
        if (failure == 2) mock.corruptRead = 3;
        REQUIRE(!LHRestoreHookPointers());
        REQUIRE(*oldptr == (void *)0x11110000 && mock.reads == 4);
        finishAndRequire(true);
    }
}

// PC 和可见 LR 只在覆盖区内部拒绝，首指令与紧邻末端保持合法边界。
static void testThreadEntryBoundaries(void) {
    const uintptr_t addresses[] = {0, 0x0ffc, 0x1000, 0x1004, 0x1008, 0x100c, 0x1010};
    for (unsigned kind = 0; kind < 2; kind++) {
        for (unsigned i = 0; i < sizeof(addresses) / sizeof(*addresses); i++) {
            resetFixture();
            queueResource(NULL, 0x22220000, 12);
            thread_t threads[] = {1, MACH_PORT_NULL, 2};
            struct LHThreadPause pause = {threads, 3, 0};
            if (kind == 0) mock.pc[1] = addresses[i];
            else mock.lr[1] = addresses[i];
            bool allowed = addresses[i] <= 0x1000 || addresses[i] >= 0x100c;
            REQUIRE(LHValidateHookResources(&pause) == allowed);
            finishAndRequire(false);
        }
    }
}

// 线程状态读取失败和短状态长度拒绝；已终止线程不会阻止其他资源验证。
static void testThreadStateFailures(void) {
    const kern_return_t results[] = {KERN_FAILURE, KERN_INVALID_ARGUMENT, KERN_TERMINATED, MACH_SEND_INVALID_DEST};
    for (unsigned i = 0; i < sizeof(results) / sizeof(*results); i++) {
        resetFixture();
        queueResource(NULL, 0x22220000, 12);
        thread_t thread = 1;
        struct LHThreadPause pause = {&thread, 1, 0};
        mock.stateResult[1] = results[i];
        REQUIRE(LHValidateHookResources(&pause) == (i >= 2));
        finishAndRequire(false);
    }
    resetFixture();
    queueResource(NULL, 0x22220000, 12);
    thread_t thread = 1;
    struct LHThreadPause pause = {&thread, 1, 0};
    mock.stateCount--;
    REQUIRE(!LHValidateHookResources(&pause));
    finishAndRequire(false);
}

// 单指令补丁无需多指令区间检查，但任何 header relay 复核失败仍拒绝。
static void testSingleInstructionAndHeaderValidation(void) {
    resetFixture();
    queueResource(NULL, 0x22220000, 4);
    thread_t thread = 1;
    struct LHThreadPause pause = {&thread, 1, 0};
    mock.stateResult[1] = KERN_FAILURE;
    REQUIRE(LHValidateHookResources(&pause) && !mock.stateReads);
    mock.headerValid = false;
    REQUIRE(!LHValidateHookResources(&pause) && !mock.stateReads);
    finishAndRequire(false);
}

// 调用真实资源实现执行发布与恢复回归，所有页和输出槽均由测试私有持有。
int main(void) {
    REQUIRE(!posix_memalign((void **)&slots, PAGE_SIZE, PAGE_SIZE * 2));
    testPointerSnapshotAndRestore();
    testRepeatedOutputSlot();
    testSuccessfulResourceRetention();
    testPreparationFailures();
    testPartiallyOverlappingSlots();
    testDirtyRangeBoundaries();
    testRestoreFailuresRetainResources();
    testRestoreContinuesAfterFailure();
    testRepeatedSlotRestoreFailure();
    testThreadEntryBoundaries();
    testThreadStateFailures();
    testSingleInstructionAndHeaderValidation();
    resetFixture();
    free(slots);
    printf("PASS Hook pointer publication, rollback, resource lifetime and PC/LR boundaries: %u checks\n", checks);
    return 0;
}
