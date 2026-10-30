// 用内核接口桩验证事务快照、跨页失败和提交边界，不修改宿主机代码页。
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach_vm.h>
#include <new>
#include <vector>
#include "../libhooker/mem/shadowMem.h"
#include "../libhooker/mem/threadSafety.h"
#include "../libhooker/mem/hook-transaction.h"
#include "../libhooker/mem/header-relay.h"
extern "C" {
#include "../libhooker/mem/writeMem.h"
#include "../libhooker/include/libhooker.h"
}

static unsigned checks;
#define REQUIRE(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "shadow-memory failure at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

static uint8_t *memory;
static size_t memorySize;
struct MockState {
    unsigned reads = 0;
    unsigned pauses = 0;
    unsigned commits = 0;
    unsigned resumes = 0;
    unsigned finishes = 0;
    unsigned publications = 0;
    unsigned restores = 0;
    unsigned pointerPreparations = 0;
    unsigned pointerRestores = 0;
    unsigned committedPages[6] = {};
    bool committedRelayPhase[6] = {};
    unsigned committedBytes[6] = {};
    unsigned commitEvents[6] = {};
    unsigned relayWrites[3] = {};
    unsigned restoredPages[6] = {};
    bool restoredRelayPhase[6] = {};
    unsigned restoredBytes[6] = {};
    unsigned restoreEvents[6] = {};
    unsigned eventCounter = 0;
    unsigned publicationEvent = 0;
    unsigned pointerRestoreEvent = 0;
    size_t relayStarts[3] = {};
    size_t relaySizes[3] = {};
    uint32_t protections[3] = {VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ, VM_PROT_READ | VM_PROT_EXECUTE};
    int failedCommitPage = -1;
    int failedRelayCommitPage = -1;
    int failedRestorePage = -1;
    int failedRelayRestorePage = -1;
    int unreadableProtectionPage = -1;
    unsigned failedWriteAmount = 0;
    bool validationSucceeds = true;
    bool pointerPreparationSucceeds = true;
    bool pointerRestoreSucceeds = true;
    bool hasHookPointer = false;
    bool verifyReadyRelays = false;
    bool mutateAfterRelayCommit = false;
    void *oldPointer = (void *)0x11220000;
    void *savedPointer = nullptr;
    void **pointerSlot = nullptr;
    unsigned failedReadCall = 0;
    bool preserved = false;
    uintptr_t unreadablePage = 0;
    bool shortRead = false;
    bool pauseSucceeds = true;
};
static MockState mock;
static bool failAllocations;

// 仅在指定回归中拒绝 C++ 分配，验证 map 创建和插入异常的恢复路径。
void *operator new(size_t size) {
    if (failAllocations) throw std::bad_alloc();
    void *allocation = malloc(size ? size : 1);
    if (!allocation) throw std::bad_alloc();
    return allocation;
}

// 与测试分配器配对，保持普通测试中的节点释放行为。
void operator delete(void *allocation) noexcept { free(allocation); }

// 模拟按页读取，支持不可读页及“成功但长度不足”的内核返回。
extern "C" kern_return_t mach_vm_read_overwrite(vm_map_read_t, mach_vm_address_t address,
        mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *copied) {
    ++mock.reads;
    REQUIRE(!(address & PAGE_MASK));
    REQUIRE(size == PAGE_SIZE);
    *copied = 0;
    if (mock.reads == mock.failedReadCall || address == mock.unreadablePage || address < (uintptr_t)memory ||
            address - (uintptr_t)memory > memorySize - PAGE_SIZE) return KERN_PROTECTION_FAILURE;
    *copied = mock.shortRead ? size - 1 : size;
    memcpy((void *)data, (const void *)address, *copied);
    return KERN_SUCCESS;
}

// 记录暂停请求，保持测试进程中的真实线程继续运行。
extern "C" bool LHPauseThreads(struct LHThreadPause *pause) {
    ++mock.pauses;
    pause->threads = NULL;
    pause->count = 0;
    pause->allocationSize = 0;
    return mock.pauseSucceeds;
}

// 记录恢复阶段，确认测试没有创建真实的 Mach 线程列表。
extern "C" void LHResumeThreads(struct LHThreadPause *pause) {
    ++mock.resumes;
    REQUIRE(!pause->threads && !pause->count && !pause->allocationSize);
}

// 控制暂停后的资源验证结果，不依赖已经删除的 PC 修复机制。
extern "C" bool LHValidateHookResources(const struct LHThreadPause *) { return mock.validationSucceeds; }

// 模拟发布前保存原调用槽，实际资源实现由独立回归验证。
extern "C" bool LHPrepareHookPointers(void) {
    ++mock.pointerPreparations;
    REQUIRE(mock.resumes == 0);
    mock.savedPointer = mock.pointerSlot ? *mock.pointerSlot : mock.oldPointer;
    return mock.pointerPreparationSucceeds;
}

// 记录入口提交前的原调用发布阶段。
extern "C" void LHPublishHookPointers(void) {
    ++mock.publications;
    mock.publicationEvent = ++mock.eventCounter;
    if (mock.verifyReadyRelays) {
        for (unsigned page = 0; page < 3; page++) {
            if (!mock.relaySizes[page]) continue;
            REQUIRE(mock.relayWrites[page] == 1);
            for (size_t offset = 133; offset < 138; offset++)
                REQUIRE(memory[page * PAGE_SIZE + offset] == 0xa5);
        }
    }
    if (mock.hasHookPointer) mock.oldPointer = (void *)0x33440000;
    if (mock.pointerSlot) *mock.pointerSlot = (void *)0x33440000;
}

// 非 relay 字节恢复后才还原调用槽，并记录它与两轮页面恢复的先后关系。
extern "C" bool LHRestoreHookPointers(void) {
    ++mock.pointerRestores;
    mock.pointerRestoreEvent = ++mock.eventCounter;
    REQUIRE(mock.resumes == 0);
    if (!mock.pointerRestoreSucceeds) return false;
    if (mock.hasHookPointer) mock.oldPointer = mock.savedPointer;
    if (mock.pointerSlot) *mock.pointerSlot = mock.savedPointer;
    return true;
}

// 记录资源处理结果，不依赖已经移除的 PC 修复队列。
extern "C" void LHFinishHookResources(bool preserve) {
    ++mock.finishes;
    mock.preserved = preserve;
}

// 模拟待发布的头部中继范围；真实实现由独立头部中继回归检查。
extern "C" void LHGetPendingHeaderRelayMask(uintptr_t address, uint8_t *mask, size_t pageSize) {
    REQUIRE(mock.pauses == 0 && pageSize == PAGE_SIZE);
    REQUIRE(address >= (uintptr_t)memory && address - (uintptr_t)memory < memorySize);
    unsigned page = (unsigned)((address - (uintptr_t)memory) / PAGE_SIZE);
    memset(mask, 0, pageSize / 8);
    REQUIRE(mock.relayStarts[page] + mock.relaySizes[page] <= pageSize);
    for (size_t offset = mock.relayStarts[page]; offset < mock.relayStarts[page] + mock.relaySizes[page]; offset++)
        mask[offset / 8] |= (uint8_t)(1u << (offset % 8));
}

// 查询单页原权限，允许注入权限快照失败。
extern "C" bool LHGetMemoryProtection(uintptr_t destination, uint32_t *protection) {
    REQUIRE(!(destination & PAGE_MASK));
    REQUIRE(destination >= (uintptr_t)memory && destination - (uintptr_t)memory < memorySize);
    unsigned page = (unsigned)((destination - (uintptr_t)memory) / PAGE_SIZE);
    if ((int)page == mock.unreadableProtectionPage) return false;
    *protection = mock.protections[page];
    return true;
}

// 仅写回测试分配的页，并检查提交严格发生在一次暂停请求之后。
extern "C" bool LHCommitMemoryRanges(uintptr_t destination, const void *data, size_t size,
        const uint8_t *mask, uint32_t protection) {
    unsigned page = (unsigned)((destination - (uintptr_t)memory) / PAGE_SIZE);
    REQUIRE(mock.commits < 6);
    REQUIRE(mock.pauses == 1 && mock.pauseSucceeds);
    REQUIRE(!(destination & PAGE_MASK) && size == PAGE_SIZE);
    REQUIRE(destination >= (uintptr_t)memory &&
        destination - (uintptr_t)memory <= memorySize - PAGE_SIZE);
    REQUIRE(mask != NULL);
    REQUIRE(protection == mock.protections[page]);
    unsigned relayBytes = 0, targetBytes = 0;
    bool unrelatedBytesMatch = true;
    for (size_t offset = 0; offset < size; offset++) {
        if (!(mask[offset / 8] & (1u << (offset % 8)))) {
            unrelatedBytesMatch &= ((const uint8_t *)data)[offset] == ((const uint8_t *)destination)[offset];
            continue;
        }
        bool relay = offset >= mock.relayStarts[page] && offset - mock.relayStarts[page] < mock.relaySizes[page];
        if (relay) relayBytes++; else targetBytes++;
    }
    REQUIRE((relayBytes != 0) != (targetBytes != 0));
    REQUIRE(unrelatedBytesMatch);
    bool relayPhase = relayBytes != 0;
    REQUIRE(mock.publications == (relayPhase ? 0 : 1));
    mock.committedPages[mock.commits] = page;
    mock.committedRelayPhase[mock.commits] = relayPhase;
    mock.committedBytes[mock.commits] = relayBytes + targetBytes;
    mock.commitEvents[mock.commits++] = ++mock.eventCounter;
    if (relayPhase) ++mock.relayWrites[page];
    bool failed = (int)page == (relayPhase ? mock.failedRelayCommitPage : mock.failedCommitPage);
    size_t amount = failed ? (mock.failedWriteAmount == 2 ? size : mock.failedWriteAmount == 1 ? size / 2 : 0) : size;
    bool writableData = (protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == VM_PROT_WRITE;
    // 逐字节参考实现不复用生产代码的范围合并算法。
    for (size_t offset = 0; offset < amount; offset++)
        if (!writableData || (mask[offset / 8] & (1u << (offset % 8))))
            ((uint8_t *)destination)[offset] = ((const uint8_t *)data)[offset];
    if (relayPhase && mock.mutateAfterRelayCommit) ((uint8_t *)destination)[200] = 0xb6;
    if (failed) mock.protections[page] = VM_PROT_READ | VM_PROT_EXECUTE;
    return !failed;
}

// 恢复桩只覆盖请求的脏字节，拒绝混合两个阶段的掩码或越过 oldptr 恢复边界。
extern "C" bool LHRestoreMemoryRanges(uintptr_t destination, const void *original, size_t size,
        const uint8_t *mask, uint32_t protection, void *scratch) {
    unsigned page = (unsigned)((destination - (uintptr_t)memory) / PAGE_SIZE);
    REQUIRE(mock.restores < 6 && mock.resumes == 0 && scratch && mask && original);
    REQUIRE(size == PAGE_SIZE);
    unsigned relayBytes = 0, targetBytes = 0;
    for (size_t offset = 0; offset < size; offset++) {
        if (!(mask[offset / 8] & (1u << (offset % 8)))) continue;
        bool relay = offset >= mock.relayStarts[page] && offset - mock.relayStarts[page] < mock.relaySizes[page];
        if (relay) relayBytes++; else targetBytes++;
    }
    REQUIRE((relayBytes != 0) != (targetBytes != 0));
    bool relayPhase = relayBytes != 0;
    REQUIRE(relayPhase ? (!mock.publications ? mock.pointerRestores == 0 :
        mock.pointerRestores == 1 && mock.pointerRestoreSucceeds) : mock.pointerRestores == 0);
    mock.restoredPages[mock.restores] = page;
    mock.restoredRelayPhase[mock.restores] = relayPhase;
    mock.restoredBytes[mock.restores] = relayBytes + targetBytes;
    mock.restoreEvents[mock.restores++] = ++mock.eventCounter;
    if ((int)page == mock.failedRestorePage || (relayPhase && (int)page == mock.failedRelayRestorePage)) return false;
    for (size_t offset = 0; offset < size; offset++)
        if (mask[offset / 8] & (1u << (offset % 8)))
            ((uint8_t *)destination)[offset] = ((const uint8_t *)original)[offset];
    mock.protections[page] = protection;
    return true;
}

// 在清空前一事务后恢复测试数据与计数器，防止测试之间共享快照。
static void resetFixture(void) {
    REQUIRE(LHCommitShadowPages());
    mock = MockState{};
    memset(memory, 0x35, memorySize);
}

// 初始化或插入节点内存不足时返回失败；后续调用可重试，不保留失败快照。
static void testMapAllocationFailures(void) {
    memset(memory, 0x35, memorySize);
    failAllocations = true;
    REQUIRE(!LHPrepareShadowPage((uintptr_t)memory));
    failAllocations = false;
    REQUIRE(LHPrepareShadowPage((uintptr_t)memory));
    REQUIRE(LHCommitShadowPages());
    resetFixture();
    failAllocations = true;
    REQUIRE(!LHPrepareShadowPage((uintptr_t)memory));
    failAllocations = false;
    REQUIRE(mock.reads == 1);
    REQUIRE(LHPrepareShadowPage((uintptr_t)memory));
    REQUIRE(mock.reads == 2);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 0 && mock.commits == 0);
}

// 同页读取只访问内核一次，纯读取提交既不暂停也不恢复旧数据。
static void testReadCache(void) {
    resetFixture();
    uint32_t original = 0x11223344, changed = 0xaabbccdd, value = 0;
    memcpy(memory + 16, &original, sizeof(original));
    REQUIRE(LHCopyMemPage(&value, (uintptr_t)memory, 16, sizeof(value)));
    REQUIRE(value == original && mock.reads == 1);
    memcpy(memory + 16, &changed, sizeof(changed));
    REQUIRE(LHCopyMemPage(&value, (uintptr_t)memory, 16, sizeof(value)));
    REQUIRE(value == original && mock.reads == 1);
    REQUIRE(LHPrepareShadowPage((uintptr_t)memory));
    REQUIRE(mock.reads == 1);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 0 && mock.commits == 0 && mock.finishes == 1);
    REQUIRE(!memcmp(memory + 16, &changed, sizeof(changed)));
    REQUIRE(LHCopyMemPage(&value, (uintptr_t)memory, 16, sizeof(value)));
    REQUIRE(value == changed && mock.reads == 2);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 0 && mock.commits == 0);
}

// 第二页不可读时，第一页快照和真实内存都不能留下失败补丁的一部分。
static void testCrossPageFailure(void) {
    resetFixture();
    mock.unreadablePage = (uintptr_t)memory + PAGE_SIZE;
    const uint64_t replacement = UINT64_C(0x123456789abcdef0);
    REQUIRE(!LHWriteMemoryInternal(memory + PAGE_SIZE - 4, &replacement, sizeof(replacement)));
    REQUIRE(mock.reads == 2);
    uint32_t tail = 0;
    REQUIRE(LHCopyMemPage(&tail, (uintptr_t)memory, PAGE_SIZE - 4, sizeof(tail)));
    REQUIRE(tail == 0x35353535 && mock.reads == 2);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 0 && mock.commits == 0);
    std::vector<uint8_t> expected(memorySize, 0x35);
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
}

// 同批前一个补丁成功、后一个跨页失败时，只允许提交前一个补丁。
static void testEarlierPatchSurvivesFailure(void) {
    resetFixture();
    mock.unreadablePage = (uintptr_t)memory + PAGE_SIZE;
    uint32_t accepted = 0x11223344;
    uint64_t rejected = UINT64_C(0x123456789abcdef0);
    struct LHMemoryPatch patches[] = {
        {memory + 8, &accepted, sizeof(accepted), NULL},
        {memory + PAGE_SIZE - 4, &rejected, sizeof(rejected), NULL}
    };
    REQUIRE(LHPatchMemory(patches, 2) == 1);
    REQUIRE(errno == LIBHOOKER_ERR_VM);
    REQUIRE(mock.reads == 4 && mock.pauses == 1 && mock.commits == 1);
    std::vector<uint8_t> expected(memorySize, 0x35);
    memcpy(expected.data() + 8, &accepted, sizeof(accepted));
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
}

// 待提交写入必须立即出现在同一事务的读取中，但提交前不改变目标页。
static void testDirtyReadView(void) {
    resetFixture();
    uint32_t replacement = 0x12345678;
    LHLockMemory();
    uint32_t untouched = 0;
    REQUIRE(LHCopyMemPage(&untouched, (uintptr_t)memory + 2 * PAGE_SIZE, 0, sizeof(untouched)));
    REQUIRE(untouched == 0x35353535);
    memory[2 * PAGE_SIZE] = 0xa5;
    REQUIRE(LHWriteMemoryInternal(memory + 24, &replacement, sizeof(replacement)));
    REQUIRE(mock.reads == 2 && mock.commits == 0);
    REQUIRE(memory[24] == 0x35);
    void *copy = LHReadShadowMem(memory + 24, sizeof(replacement));
    REQUIRE(copy && !memcmp(copy, &replacement, sizeof(replacement)));
    free(copy);
    REQUIRE(mock.reads == 2);
    REQUIRE(LHCommitShadowPages());
    LHUnlockMemory();
    REQUIRE(mock.pauses == 1 && mock.commits == 1);
    REQUIRE(!memcmp(memory + 24, &replacement, sizeof(replacement)));
    REQUIRE(memory[2 * PAGE_SIZE] == 0xa5);
}

// 三页成功写入检查首尾部分页、整页中段以及原有未修改字节。
static void testCrossPageSuccess(void) {
    resetFixture();
    std::vector<uint8_t> patch(PAGE_SIZE + 16);
    for (size_t i = 0; i < patch.size(); i++) patch[i] = (uint8_t)(i * 17 + 9);
    std::vector<uint8_t> expected(memorySize, 0x35);
    memcpy(expected.data() + PAGE_SIZE - 8, patch.data(), patch.size());
    REQUIRE(LHWriteMemoryInternal(memory + PAGE_SIZE - 8, patch.data(), patch.size()));
    REQUIRE(mock.reads == 3 && mock.commits == 0);
    void *copy = LHReadShadowMem(memory + PAGE_SIZE - 8, patch.size());
    REQUIRE(copy && !memcmp(copy, patch.data(), patch.size()));
    free(copy);
    REQUIRE(mock.reads == 3);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.reads == 9 && mock.pauses == 1 && mock.commits == 3);
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
}

// 长度不足的快照必须丢弃，重试读取会重新请求内核而不会命中坏缓存。
static void testShortRead(void) {
    resetFixture();
    mock.shortRead = true;
    uint32_t output = 0xdeadbeef;
    REQUIRE(!LHCopyMemPage(&output, (uintptr_t)memory, 0, sizeof(output)));
    REQUIRE(output == 0xdeadbeef && mock.reads == 1);
    mock.shortRead = false;
    REQUIRE(LHCopyMemPage(&output, (uintptr_t)memory, 0, sizeof(output)));
    REQUIRE(output == 0x35353535 && mock.reads == 2);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 0 && mock.commits == 0);
}

// 暂停失败保持现有语义：返回失败，不提交数据，并清除本次事务快照。
static void testPauseFailure(void) {
    resetFixture();
    uint32_t replacement = 0x12345678;
    REQUIRE(LHWriteMemoryInternal(memory + 8, &replacement, sizeof(replacement)));
    mock.pauseSucceeds = false;
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(mock.pauses == 1 && mock.commits == 0 && memory[8] == 0x35);
    uint32_t output = 0;
    REQUIRE(LHCopyMemPage(&output, (uintptr_t)memory, 8, sizeof(output)));
    REQUIRE(output == 0x35353535 && mock.reads == 2);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 1 && mock.commits == 0);
}

// 暂存后同页其他数据仍可能变化，提交只覆盖明确要求写入的字节。
static void testUnrelatedWritesSurvive(void) {
    resetFixture();
    const uint8_t first[] = {0x11, 0x22, 0x33};
    const uint8_t second[] = {0x44, 0x55, 0x66, 0x77};
    REQUIRE(LHWriteMemoryInternal(memory + 7, first, sizeof(first)));
    memory[6] = 0xa1;
    memory[10] = 0xa2;
    memory[16] = 0xa3;
    REQUIRE(LHWriteMemoryInternal(memory + 23, second, sizeof(second)));
    memory[22] = 0xa4;
    memory[27] = 0xa5;
    // 目标范围即使被并发改写，最终仍应以本批补丁为准。
    memory[8] = 0xff;
    memory[25] = 0xff;
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.reads == 3 && mock.pauses == 1 && mock.commits == 1);
    REQUIRE(!memcmp(memory + 7, first, sizeof(first)));
    REQUIRE(!memcmp(memory + 23, second, sizeof(second)));
    REQUIRE(memory[6] == 0xa1 && memory[10] == 0xa2 && memory[16] == 0xa3);
    REQUIRE(memory[22] == 0xa4 && memory[27] == 0xa5);
}

// 重叠补丁以最后一次写入为准，且不会扩大首尾位图的实际目标范围。
static void testOverlappingWrites(void) {
    resetFixture();
    const uint8_t first[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const uint8_t second[] = {21, 22, 23, 24, 25, 26};
    uint8_t expected[sizeof(first)];
    memcpy(expected, first, sizeof(first));
    memcpy(expected + 4, second, sizeof(second));
    REQUIRE(LHWriteMemoryInternal(memory + 5, first, sizeof(first)));
    REQUIRE(LHWriteMemoryInternal(memory + 9, second, sizeof(second)));
    memory[4] = 0xb1;
    memory[15] = 0xb2;
    void *copy = LHReadShadowMem(memory + 5, sizeof(expected));
    REQUIRE(copy && !memcmp(copy, expected, sizeof(expected)));
    free(copy);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(!memcmp(memory + 5, expected, sizeof(expected)));
    REQUIRE(memory[4] == 0xb1 && memory[15] == 0xb2);
}

// 用逐字节参考数组核对目标范围，不复用生产位图或合并算法。
static void runRangeCase(size_t offset, size_t size, bool overlap) {
    resetFixture();
    std::vector<uint8_t> first(size);
    for (size_t i = 0; i < size; i++) first[i] = (uint8_t)(i * 13 + 7);
    REQUIRE(LHWriteMemoryInternal(memory + offset, first.data(), size));
    for (size_t i = 0; i < memorySize; i++) memory[i] = (uint8_t)(i * 7 + 23);
    size_t secondOffset = offset + size / 2;
    size_t secondSize = size + 3;
    if (secondSize > PAGE_SIZE - secondOffset) secondSize = PAGE_SIZE - secondOffset;
    std::vector<uint8_t> second(overlap ? secondSize : 0);
    if (overlap) {
        for (size_t i = 0; i < secondSize; i++) second[i] = (uint8_t)(i * 11 + 71);
        REQUIRE(LHWriteMemoryInternal(memory + secondOffset, second.data(), secondSize));
        for (size_t i = 0; i < memorySize; i++) memory[i] = (uint8_t)(i * 17 + 53);
    }
    std::vector<uint8_t> expected(memory, memory + memorySize);
    memcpy(expected.data() + offset, first.data(), size);
    if (overlap) memcpy(expected.data() + secondOffset, second.data(), secondSize);
    void *copy = LHReadShadowMem(memory + offset, size);
    REQUIRE(copy && !memcmp(copy, expected.data() + offset, size));
    free(copy);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
    REQUIRE(mock.reads == 3 && mock.pauses == 1 && mock.commits == 1);
}

// 覆盖每种位内起点、短范围跨组、精确页尾、整页和同批覆盖。
static void testRangeBoundaries(void) {
    for (size_t bit = 0; bit < 8; bit++) {
        for (size_t size = 1; size <= 17; size++) {
            runRangeCase(32 + bit, size, false);
            runRangeCase(32 + bit, size, true);
        }
    }
    for (size_t size = 1; size <= 17; size++) {
        runRangeCase(PAGE_SIZE - size, size, false);
        runRangeCase(PAGE_SIZE - size, size, true);
    }
    runRangeCase(0, PAGE_SIZE, false);
    runRangeCase(0, PAGE_SIZE, true);
}

// 刷新阶段第二页不可读时，任何页都不得进入底层提交。
static void testRefreshFailure(void) {
    resetFixture();
    uint64_t patch = UINT64_C(0x123456789abcdef0);
    REQUIRE(LHWriteMemoryInternal(memory + PAGE_SIZE - 4, &patch, sizeof(patch)));
    memory[0] = 0xa1;
    memory[PAGE_SIZE + 8] = 0xa2;
    mock.unreadablePage = (uintptr_t)memory + PAGE_SIZE;
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(mock.pauses == 1 && mock.commits == 0 && mock.reads == 4);
    REQUIRE(memory[0] == 0xa1 && memory[PAGE_SIZE + 8] == 0xa2);
    for (size_t i = PAGE_SIZE - 4; i < PAGE_SIZE + 4; i++) REQUIRE(memory[i] == 0x35);
    mock.unreadablePage = 0;
    uint64_t unchanged = 0;
    void *copy = LHReadShadowMem(memory + PAGE_SIZE - 4, sizeof(unchanged));
    REQUIRE(copy);
    memcpy(&unchanged, copy, sizeof(unchanged));
    free(copy);
    REQUIRE(unchanged == UINT64_C(0x3535353535353535));
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.commits == 0);
}

// 刷新读取即使返回成功也必须覆盖整页，否则不能提交部分有效的数据。
static void testShortRefresh(void) {
    resetFixture();
    const uint8_t replacement = 0x77;
    REQUIRE(LHWriteMemoryInternal(memory + 8, &replacement, sizeof(replacement)));
    mock.shortRead = true;
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(mock.reads == 2 && mock.commits == 0 && memory[8] == 0x35);
    mock.shortRead = false;
    REQUIRE(LHCommitShadowPages());
}

// 拒绝非法范围及整数溢出，确保这些请求不会触发底层读取或提交。
static void testInvalidBounds(void) {
    resetFixture();
    uint32_t value = 0;
    REQUIRE(!LHCopyMemPage(NULL, (uintptr_t)memory, 0, sizeof(value)));
    REQUIRE(!LHCopyMemPage(&value, (uintptr_t)memory, PAGE_SIZE + 1, 0));
    REQUIRE(!LHCopyMemPage(&value, (uintptr_t)memory, PAGE_SIZE - 1, 2));
    REQUIRE(!LHCopyMemPage(&value, (uintptr_t)memory, 1, SIZE_MAX));
    REQUIRE(!LHCopyMemPage(&value, (uintptr_t)memory + 1, 0, sizeof(value)));
    REQUIRE(!LHPrepareShadowPage((uintptr_t)memory + 1));
    REQUIRE(!LHWriteShadowPage((uintptr_t)memory, 0, &value, sizeof(value)));
    REQUIRE(!LHWriteShadowPage((uintptr_t)memory, PAGE_SIZE, &value, sizeof(value)));
    REQUIRE(!LHWriteShadowPage((uintptr_t)memory, 0, NULL, sizeof(value)));
    REQUIRE(!LHWriteShadowPage((uintptr_t)memory, 0, &value, 0));
    REQUIRE(!LHReadShadowMem(NULL, 1));
    REQUIRE(!LHReadShadowMem(memory, 0));
    REQUIRE(!LHReadShadowMem((void *)(UINTPTR_MAX - 3), 8));
    REQUIRE(!LHWriteMemoryInternal(NULL, &value, sizeof(value)));
    REQUIRE(!LHWriteMemoryInternal(memory, NULL, sizeof(value)));
    REQUIRE(!LHWriteMemoryInternal(memory, &value, 0));
    REQUIRE(!LHWriteMemoryInternal((void *)(UINTPTR_MAX - 3), &value, 8));
    REQUIRE(mock.reads == 0);
    REQUIRE(LHCommitShadowPages());
    REQUIRE(mock.pauses == 0 && mock.commits == 0);
}

// 在三页中段写入，保留首尾非目标字节并制造准备后发生的新内容。
static std::vector<uint8_t> stageRollbackFixture(void) {
    const size_t offset = PAGE_SIZE / 2 - 5;
    std::vector<uint8_t> patch(PAGE_SIZE * 2 + 10, 0x91);
    REQUIRE(LHWriteMemoryInternal(memory + offset, patch.data(), patch.size()));
    memory[0] = 0x72;
    memory[memorySize - 1] = 0x73;
    memory[offset + 1] = 0x74;
    mock.hasHookPointer = true;
    return std::vector<uint8_t>(memory, memory + memorySize);
}

// 首、中、末页分别在未写、半写、全部写后报错，所有已尝试页都必须逆序恢复。
static void testCommitFailureRollback(void) {
    for (int failedPage = 0; failedPage < 3; failedPage++) {
        for (unsigned amount = 0; amount < 3; amount++) {
            resetFixture();
            std::vector<uint8_t> expected = stageRollbackFixture();
            uint32_t originalProtections[3];
            memcpy(originalProtections, mock.protections, sizeof(originalProtections));
            mock.failedCommitPage = failedPage;
            mock.failedWriteAmount = amount;
            errno = 0;
            REQUIRE(!LHCommitShadowPages());
            REQUIRE(errno == LIBHOOKER_ERR_VM);
            REQUIRE(mock.commits == (unsigned)failedPage + 1);
            REQUIRE(mock.restores == (unsigned)failedPage + 1);
            for (unsigned index = 0; index < mock.commits; index++) {
                REQUIRE(mock.committedPages[index] == index);
                REQUIRE(mock.restoredPages[index] == (unsigned)failedPage - index);
            }
            REQUIRE(!memcmp(memory, expected.data(), memorySize));
            REQUIRE(!memcmp(mock.protections, originalProtections, sizeof(originalProtections)));
            REQUIRE(mock.pointerRestores == 1 && mock.oldPointer == (void *)0x11220000);
            REQUIRE(!mock.preserved && mock.resumes == 1 && mock.finishes == 1);
            REQUIRE(LHCommitShadowPages());
            REQUIRE(mock.commits == (unsigned)failedPage + 1);
        }
    }
}

// 恢复遇首个错误立即停止，不能再清掉仍被高地址入口依赖的低地址 relay。
static void testRollbackFailurePreservesResources(void) {
    for (int failedPage = 0; failedPage < 3; failedPage++) {
        resetFixture();
        stageRollbackFixture();
        mock.failedCommitPage = 2;
        mock.failedWriteAmount = 1;
        mock.failedRestorePage = failedPage;
        errno = 0;
        REQUIRE(!LHCommitShadowPages());
        REQUIRE(errno == LIBHOOKER_ERR_ROLLBACK);
        REQUIRE(mock.restores == (unsigned)(3 - failedPage));
        for (unsigned index = 0; index < mock.restores; index++)
            REQUIRE(mock.restoredPages[index] == 2 - index);
        REQUIRE(mock.pointerRestores == 0 && mock.oldPointer == (void *)0x33440000);
        REQUIRE(mock.preserved && mock.resumes == 1 && mock.finishes == 1);
    }
}

// 只有原调用槽也恢复成功才能释放资源，恢复失败应明确保留 published 指针。
static void testPointerRestorationFailure(void) {
    resetFixture();
    std::vector<uint8_t> expected = stageRollbackFixture();
    mock.failedCommitPage = 1;
    mock.failedWriteAmount = 1;
    mock.pointerRestoreSucceeds = false;
    errno = 0;
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(errno == LIBHOOKER_ERR_ROLLBACK);
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
    REQUIRE(mock.pointerRestores == 1 && mock.oldPointer == (void *)0x33440000);
    REQUIRE(mock.preserved && mock.resumes == 1);
}

// 模拟低地址入口经 original 依赖更高地址镜像中继；中继边缘含未改写的预留字节。
static std::vector<uint8_t> stageRelayChain(bool mixedPage, bool multipleRelays) {
    const uint8_t target[8] = {0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91};
    const uint8_t relay[5] = {0xa5, 0xa5, 0xa5, 0xa5, 0xa5};
    for (unsigned page : {0u, 2u})
        REQUIRE(LHWriteMemoryInternal(memory + page * PAGE_SIZE + 64, target, sizeof(target)));
    for (unsigned page = 0; page < 3; page++) {
        if (page != 1 && !multipleRelays) continue;
        mock.relayStarts[page] = 131;
        mock.relaySizes[page] = 9;
        REQUIRE(LHWriteMemoryInternal(memory + page * PAGE_SIZE + 133, relay, sizeof(relay)));
        // 这些字节属于预留区，但不属于 dirty mask，两个恢复阶段都不得触碰。
        memory[page * PAGE_SIZE + 132] = 0x72;
        memory[page * PAGE_SIZE + 138] = 0x73;
    }
    if (mixedPage) {
        // 与 relay 共用位图字节，确保按位分离，不能按整组或整页归类。
        REQUIRE(LHWriteMemoryInternal(memory + PAGE_SIZE + 128, target, 3));
        REQUIRE(LHWriteMemoryInternal(memory + PAGE_SIZE + 140, target, 4));
    }
    mock.hasHookPointer = true;
    mock.verifyReadyRelays = true;
    mock.failedCommitPage = 2;
    mock.failedWriteAmount = 2;
    return std::vector<uint8_t>(memory, memory + memorySize);
}

// 任一 relay 预提交失败都不能发布 oldptr 或入口；恢复失败也不会留下新输出槽。
static void testRelayPrecommitFailure(void) {
    for (int failedPage = 0; failedPage < 3; failedPage++) {
        for (unsigned amount = 0; amount < 3; amount++) {
            for (unsigned restoreFailure = 0; restoreFailure < 2; restoreFailure++) {
                resetFixture();
                std::vector<uint8_t> expected = stageRelayChain(true, true);
                uint32_t originalProtections[3];
                memcpy(originalProtections, mock.protections, sizeof(originalProtections));
                mock.failedRelayCommitPage = failedPage;
                mock.failedWriteAmount = amount;
                if (restoreFailure) mock.failedRelayRestorePage = failedPage;
                REQUIRE(!LHCommitShadowPages());
                REQUIRE(errno == (restoreFailure ? LIBHOOKER_ERR_ROLLBACK : LIBHOOKER_ERR_VM));
                REQUIRE(mock.commits == (unsigned)failedPage + 1);
                REQUIRE(mock.restores == (restoreFailure ? 1 : (unsigned)failedPage + 1));
                REQUIRE(mock.publications == 0 && mock.pointerRestores == 0);
                REQUIRE(mock.oldPointer == (void *)0x11220000);
                for (unsigned index = 0; index < mock.commits; index++) {
                    REQUIRE(mock.committedRelayPhase[index] && mock.committedPages[index] == index);
                    REQUIRE(mock.committedBytes[index] == 5 && mock.relayWrites[index] == 1);
                }
                for (unsigned index = 0; index < mock.restores; index++) {
                    REQUIRE(mock.restoredRelayPhase[index]);
                    REQUIRE(mock.restoredPages[index] == (unsigned)failedPage - index);
                }
                if (restoreFailure) {
                    for (int page = 0; page <= failedPage; page++)
                        if (page != failedPage || amount)
                            memset(expected.data() + page * PAGE_SIZE + 133, 0xa5, 5);
                } else {
                    REQUIRE(!memcmp(mock.protections, originalProtections, sizeof(originalProtections)));
                }
                REQUIRE(!memcmp(memory, expected.data(), memorySize));
                REQUIRE(mock.preserved == (restoreFailure != 0));
                REQUIRE(mock.resumes == 1 && mock.finishes == 1);
            }
        }
    }
}

// 最低入口的写入和恢复都失败时，尚未处理的高镜像也已具备完整 relay。
static void testReadyRelaysSurviveEarlyTargetFailure(void) {
    resetFixture();
    std::vector<uint8_t> expected = stageRelayChain(true, true);
    mock.failedCommitPage = 0;
    mock.failedRestorePage = 0;
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(errno == LIBHOOKER_ERR_ROLLBACK);
    REQUIRE(mock.commits == 4 && mock.restores == 1);
    for (unsigned page = 0; page < 3; page++) {
        REQUIRE(mock.committedRelayPhase[page] && mock.committedPages[page] == page);
        REQUIRE(mock.commitEvents[page] < mock.publicationEvent && mock.relayWrites[page] == 1);
        memset(expected.data() + page * PAGE_SIZE + 133, 0xa5, 5);
    }
    REQUIRE(!mock.committedRelayPhase[3] && mock.committedPages[3] == 0);
    REQUIRE(mock.commitEvents[3] > mock.publicationEvent);
    REQUIRE(!mock.restoredRelayPhase[0] && mock.restoredPages[0] == 0);
    REQUIRE(mock.pointerRestores == 0 && mock.oldPointer == (void *)0x33440000);
    memset(expected.data() + 64, 0x91, 8);
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
    REQUIRE(mock.preserved && mock.resumes == 1 && mock.finishes == 1);
}

// 同页两个阶段只各写自己的位图；第二阶段保留 relay、刚发布 oldptr 和实时非目标字节。
static void testRelayForwardMasksAndSingleWrite(void) {
    for (unsigned executable = 0; executable < 2; executable++) {
        resetFixture();
        mock.protections[1] = VM_PROT_READ | VM_PROT_WRITE | (executable ? VM_PROT_EXECUTE : 0);
        mock.pointerSlot = (void **)(memory + PAGE_SIZE + 64);
        *mock.pointerSlot = (void *)0x11220000;
        std::vector<uint8_t> expected = stageRelayChain(true, true);
        mock.failedCommitPage = -1;
        mock.mutateAfterRelayCommit = true;
        REQUIRE(LHCommitShadowPages());
        REQUIRE(mock.commits == 6 && mock.restores == 0 && mock.pointerRestores == 0);
        REQUIRE(mock.publications == 1 && mock.preserved);
        for (unsigned index = 0; index < 6; index++) {
            REQUIRE(mock.committedPages[index] == index % 3);
            REQUIRE(mock.committedRelayPhase[index] == (index < 3));
            REQUIRE(mock.committedBytes[index] == (index < 3 ? 5 : index == 4 ? 7 : 8));
            REQUIRE(index < 3 ? mock.commitEvents[index] < mock.publicationEvent :
                mock.commitEvents[index] > mock.publicationEvent);
        }
        for (unsigned page = 0; page < 3; page++) {
            REQUIRE(mock.relayWrites[page] == 1);
            memset(expected.data() + page * PAGE_SIZE + 133, 0xa5, 5);
            expected[page * PAGE_SIZE + 200] = 0xb6;
        }
        for (unsigned page : {0u, 2u}) memset(expected.data() + page * PAGE_SIZE + 64, 0x91, 8);
        memset(expected.data() + PAGE_SIZE + 128, 0x91, 3);
        memset(expected.data() + PAGE_SIZE + 140, 0x91, 4);
        void *published = (void *)0x33440000;
        memcpy(expected.data() + PAGE_SIZE + 64, &published, sizeof(published));
        REQUIRE(!memcmp(memory, expected.data(), memorySize));
    }
}

// 低 target 回滚失败时，高 relay 必须仍然有效；同页普通字节可以先恢复。
static void testRelaySurvivesTargetRollbackFailure(void) {
    for (unsigned mixedPage = 0; mixedPage < 2; mixedPage++) {
        resetFixture();
        std::vector<uint8_t> expected = stageRelayChain(mixedPage, false);
        mock.failedRestorePage = 0;
        REQUIRE(!LHCommitShadowPages());
        REQUIRE(errno == LIBHOOKER_ERR_ROLLBACK);
        REQUIRE(mock.commits == 3 + mixedPage && mock.restores == 2 + mixedPage);
        REQUIRE(mock.restoredPages[0] == 2 && !mock.restoredRelayPhase[0]);
        if (mixedPage) REQUIRE(mock.restoredPages[1] == 1 && mock.restoredBytes[1] == 7);
        REQUIRE(mock.restoredPages[mock.restores - 1] == 0);
        REQUIRE(mock.pointerRestores == 0 && mock.oldPointer == (void *)0x33440000);
        memset(expected.data() + 64, 0x91, 8);
        memset(expected.data() + PAGE_SIZE + 133, 0xa5, 5);
        REQUIRE(!memcmp(memory, expected.data(), memorySize));
        REQUIRE(mock.preserved && mock.resumes == 1 && mock.finishes == 1);
    }
}

// oldptr 尚未全部恢复时，任何镜像的 relay 都不能撤销，即使普通目标已经恢复。
static void testRelaySurvivesPointerRollbackFailure(void) {
    resetFixture();
    std::vector<uint8_t> expected = stageRelayChain(true, true);
    mock.pointerRestoreSucceeds = false;
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(errno == LIBHOOKER_ERR_ROLLBACK);
    REQUIRE(mock.restores == 3 && mock.pointerRestores == 1);
    REQUIRE(mock.oldPointer == (void *)0x33440000 && mock.preserved);
    for (unsigned index = 0; index < 3; index++) {
        REQUIRE(mock.restoredPages[index] == 2 - index && !mock.restoredRelayPhase[index]);
        REQUIRE(mock.restoreEvents[index] < mock.pointerRestoreEvent);
        memset(expected.data() + index * PAGE_SIZE + 133, 0xa5, 5);
    }
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
}

// relay 阶段按地址逆序执行；任一失败保留资源并报 6，但原调用槽已经还原。
static void testRelayRollbackFailure(void) {
    for (int failedPage = 0; failedPage < 3; failedPage++) {
        resetFixture();
        std::vector<uint8_t> expected = stageRelayChain(true, true);
        mock.failedRelayRestorePage = failedPage;
        REQUIRE(!LHCommitShadowPages());
        REQUIRE(errno == LIBHOOKER_ERR_ROLLBACK);
        REQUIRE(mock.restores == (unsigned)(6 - failedPage));
        REQUIRE(mock.pointerRestores == 1 && mock.oldPointer == (void *)0x11220000);
        for (unsigned index = 0; index < mock.restores; index++) {
            bool relayPhase = index >= 3;
            REQUIRE(mock.restoredRelayPhase[index] == relayPhase);
            REQUIRE(mock.restoredPages[index] == 2 - index % 3);
            REQUIRE(relayPhase ? mock.restoreEvents[index] > mock.pointerRestoreEvent :
                mock.restoreEvents[index] < mock.pointerRestoreEvent);
            if (relayPhase) REQUIRE(mock.restoredBytes[index] == 5);
        }
        for (int page = 0; page <= failedPage; page++)
            memset(expected.data() + page * PAGE_SIZE + 133, 0xa5, 5);
        REQUIRE(!memcmp(memory, expected.data(), memorySize));
        REQUIRE(mock.preserved && mock.resumes == 1 && mock.finishes == 1);
    }
}

// 全部三阶段成功后释放资源；同页两种掩码合计只覆盖原始 dirty 字节。
static void testRelayRollbackCompletes(void) {
    resetFixture();
    std::vector<uint8_t> expected = stageRelayChain(true, true);
    uint32_t originalProtections[3];
    memcpy(originalProtections, mock.protections, sizeof(originalProtections));
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(errno == LIBHOOKER_ERR_VM && mock.restores == 6);
    REQUIRE(mock.pointerRestores == 1 && mock.oldPointer == (void *)0x11220000);
    for (unsigned index = 0; index < 6; index++) {
        REQUIRE(mock.restoredPages[index] == 2 - index % 3);
        REQUIRE(mock.restoredRelayPhase[index] == (index >= 3));
        REQUIRE(mock.restoredBytes[index] == (index >= 3 ? 5 : index == 1 ? 7 : 8));
        REQUIRE(index >= 3 ? mock.restoreEvents[index] > mock.pointerRestoreEvent :
            mock.restoreEvents[index] < mock.pointerRestoreEvent);
    }
    REQUIRE(!memcmp(memory, expected.data(), memorySize));
    REQUIRE(!memcmp(mock.protections, originalProtections, sizeof(originalProtections)));
    REQUIRE(!mock.preserved && mock.resumes == 1 && mock.finishes == 1);
}

// 权限、资源或原调用预检失败都不能开始底层写入或发布原调用。
static void testPrecommitFailures(void) {
    for (unsigned failure = 0; failure < 5; failure++) {
        resetFixture();
        std::vector<uint8_t> expected = stageRollbackFixture();
        if (failure < 3) mock.unreadableProtectionPage = (int)failure;
        if (failure == 3) mock.validationSucceeds = false;
        if (failure == 4) mock.pointerPreparationSucceeds = false;
        REQUIRE(!LHCommitShadowPages());
        REQUIRE(mock.commits == 0 && mock.restores == 0 && mock.publications == 0);
        REQUIRE(mock.oldPointer == (void *)0x11220000 && !mock.preserved);
        REQUIRE(!memcmp(memory, expected.data(), memorySize));
    }
}

// 公开批量接口必须保留提交失败与恢复失败的区别，不能无条件覆盖成普通 VM 错误。
static void testPublicRollbackErrors(void) {
    for (unsigned restoreFailure = 0; restoreFailure < 2; restoreFailure++) {
        resetFixture();
        std::vector<uint8_t> patch(memorySize, 0x93);
        struct LHMemoryPatch request = {memory, patch.data(), patch.size(), NULL};
        mock.failedCommitPage = 1;
        mock.failedWriteAmount = 1;
        if (restoreFailure) mock.failedRestorePage = 1;
        REQUIRE(LHPatchMemory(&request, 1) == 0);
        REQUIRE(errno == (restoreFailure ? LIBHOOKER_ERR_ROLLBACK : LIBHOOKER_ERR_VM));
        REQUIRE(mock.preserved == (restoreFailure != 0));
    }
}

// 发布输出槽后逐页读取实时内容，中途失败时还原已提交的前页及输出槽。
static void testRefreshAfterPublicationFailure(void) {
    resetFixture();
    std::vector<uint8_t> expected = stageRollbackFixture();
    mock.failedReadCall = 8; // 三页初读、三页撤销快照、第一页提交后读取中间页失败。
    REQUIRE(!LHCommitShadowPages());
    REQUIRE(errno == LIBHOOKER_ERR_VM);
    REQUIRE(mock.publications == 1 && mock.commits == 1 && mock.restores == 1);
    REQUIRE(mock.committedPages[0] == 0 && mock.restoredPages[0] == 0);
    REQUIRE(mock.pointerRestores == 1 && mock.oldPointer == (void *)0x11220000);
    REQUIRE(!mock.preserved && !memcmp(memory, expected.data(), memorySize));
}

// 同页非目标输出槽在 RW 与 RWX 提交中均保留已发布值，回滚后恢复事务前值。
static void testPointerOnPatchedPage(void) {
    for (unsigned executable = 0; executable < 2; executable++) {
        for (unsigned failure = 0; failure < 2; failure++) {
            resetFixture();
            mock.protections[0] = VM_PROT_READ | VM_PROT_WRITE | (executable ? VM_PROT_EXECUTE : 0);
            mock.pointerSlot = (void **)(memory + 64);
            *mock.pointerSlot = (void *)0x11220000;
            mock.hasHookPointer = true;
            const uint64_t patch = UINT64_C(0x9182736455463728);
            REQUIRE(LHWriteMemoryInternal(memory + 128, &patch, sizeof(patch)));
            REQUIRE(!LHShadowRangeIsDirty((uintptr_t)mock.pointerSlot, sizeof(void *)));
            if (failure) { mock.failedCommitPage = 0; mock.failedWriteAmount = 1; }
            REQUIRE(LHCommitShadowPages() == !failure);
            REQUIRE(*mock.pointerSlot == (failure ? (void *)0x11220000 : (void *)0x33440000));
            REQUIRE(mock.preserved == !failure);
        }
    }
}

// 查询输出槽冲突时只拒绝实际脏字节，跨页相邻地址与溢出也分别覆盖。
static void testDirtyRangeQuery(void) {
    resetFixture();
    const uint64_t patch = UINT64_C(0x9182736455463728);
    uintptr_t start = (uintptr_t)memory + PAGE_SIZE - 4;
    REQUIRE(LHWriteMemoryInternal((void *)start, &patch, sizeof(patch)));
    LHLockMemory();
    REQUIRE(!LHShadowRangeIsDirty(start - 1, 1));
    REQUIRE(LHShadowRangeIsDirty(start - 1, 2));
    REQUIRE(LHShadowRangeIsDirty(start, sizeof(patch)));
    REQUIRE(LHShadowRangeIsDirty(start + 7, 1));
    REQUIRE(!LHShadowRangeIsDirty(start + 8, sizeof(void *)));
    REQUIRE(!LHShadowRangeIsDirty(start, 0));
    REQUIRE(LHShadowRangeIsDirty(UINTPTR_MAX - 1, 8));
    LHUnlockMemory();
    REQUIRE(LHCommitShadowPages());

    // 首页面仅有干净快照，中间页未读取，仍应发现第三页的第一个脏字节。
    resetFixture();
    REQUIRE(LHPrepareShadowPage((uintptr_t)memory));
    start = (uintptr_t)memory + PAGE_SIZE - 2;
    uintptr_t dirty = (uintptr_t)memory + PAGE_SIZE * 2 + 3;
    REQUIRE(LHWriteMemoryInternal((void *)dirty, &patch, sizeof(patch)));
    LHLockMemory();
    REQUIRE(!LHShadowRangeIsDirty(start, dirty - start));
    REQUIRE(LHShadowRangeIsDirty(start, dirty - start + 1));
    REQUIRE(!LHShadowRangeIsDirty(start, 2));
    REQUIRE(!LHShadowRangeIsDirty(start + 2, PAGE_SIZE));
    REQUIRE(!LHShadowRangeIsDirty(UINTPTR_MAX, 0));
    REQUIRE(LHShadowRangeIsDirty(UINTPTR_MAX, 1));
    LHUnlockMemory();
    REQUIRE(LHCommitShadowPages());
}

// 使用真实页大小运行所有接口级回归，输出可追踪的断言总数。
int main(void) {
    memorySize = PAGE_SIZE * 3;
    REQUIRE(!posix_memalign((void **)&memory, PAGE_SIZE, memorySize));
    testMapAllocationFailures();
    testReadCache();
    testCrossPageFailure();
    testEarlierPatchSurvivesFailure();
    testDirtyReadView();
    testCrossPageSuccess();
    testShortRead();
    testPauseFailure();
    testUnrelatedWritesSurvive();
    testOverlappingWrites();
    testRangeBoundaries();
    testRefreshFailure();
    testShortRefresh();
    testInvalidBounds();
    testCommitFailureRollback();
    testRollbackFailurePreservesResources();
    testPointerRestorationFailure();
    testRelayPrecommitFailure();
    testReadyRelaysSurviveEarlyTargetFailure();
    testRelayForwardMasksAndSingleWrite();
    testRelaySurvivesTargetRollbackFailure();
    testRelaySurvivesPointerRollbackFailure();
    testRelayRollbackFailure();
    testRelayRollbackCompletes();
    testPrecommitFailures();
    testPublicRollbackErrors();
    testRefreshAfterPublicationFailure();
    testPointerOnPatchedPage();
    testDirtyRangeQuery();
    REQUIRE(LHCommitShadowPages());
    free(memory);
    printf("shadow memory tests passed (%u assertions)\n", checks);
    return 0;
}
