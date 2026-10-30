// 用 Mach 接口桩验证累计线程快照与失败清理，不暂停宿主真实线程。
#include "../libhooker/mem/threadSafety.h"
#include <mach/arm/thread_status.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 64
#define MAX_SNAPSHOTS 12
#define MAX_ALLOCATIONS 32
#define CURRENT_THREAD 1

static unsigned checks;
#define REQUIRE(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "thread-safety failure at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

struct MockThread {
    kern_return_t suspendResult;
    unsigned references, suspends, resumes, reads, releases;
    unsigned initialDepth, suspendDepth;
    bool ownedSuspension;
};
struct MockSnapshot {
    thread_t threads[MAX_THREADS];
    mach_msg_type_number_t count;
};
struct MockAllocation {
    void *address;
    vm_size_t size;
    bool cumulative;
};
static struct {
    struct MockThread threads[MAX_THREADS];
    struct MockSnapshot snapshots[MAX_SNAPSHOTS];
    struct MockAllocation allocations[MAX_ALLOCATIONS];
    unsigned snapshotCount, enumerations, enumerationFailure;
    unsigned vmAllocations, allocationFailure, snapshotReleases, listReleases;
    unsigned prepares, selfCalls, nullResumes, zeroDeallocations;
    bool prepared;
} mock;

// 用独立账本记录每次分配，核对内核快照与累计列表的释放长度。
static void *recordAllocation(vm_size_t size, bool cumulative) {
    for (unsigned i = 0; i < MAX_ALLOCATIONS; i++) {
        if (mock.allocations[i].address) continue;
        void *address = calloc(1, size);
        REQUIRE(address != NULL);
        mock.allocations[i] = (struct MockAllocation){address, size, cumulative};
        return address;
    }
    REQUIRE(false);
    return NULL;
}

// 统一检查本轮没有留下发送权、累计列表或由本事务增加的暂停计数。
static void requireClean(void) {
    for (unsigned i = 0; i < MAX_ALLOCATIONS; i++) REQUIRE(!mock.allocations[i].address);
    for (unsigned i = 1; i < MAX_THREADS; i++) {
        REQUIRE(mock.threads[i].references == 0);
        REQUIRE(!mock.threads[i].ownedSuspension);
        REQUIRE(mock.threads[i].suspendDepth == mock.threads[i].initialDepth);
    }
}

// 配置每轮可见线程，末轮快照在后续枚举中保持不变。
static void setSnapshot(unsigned round, const thread_t *threads, unsigned count) {
    REQUIRE(round < MAX_SNAPSHOTS && count < MAX_THREADS);
    mock.snapshots[round].count = count;
    memcpy(mock.snapshots[round].threads, threads, count * sizeof(*threads));
    if (mock.snapshotCount <= round) mock.snapshotCount = round + 1;
}

// 每项从稳定的四线程快照开始，不继承前项的接口失败或资源状态。
static void resetFixture(void) {
    requireClean();
    memset(&mock, 0, sizeof(mock));
    mock.prepared = true;
    const thread_t threads[] = {CURRENT_THREAD, 2, 3, 4};
    setSnapshot(0, threads, 4);
}

// 在任何枚举与暂停发生前注入后端准备失败。
bool LHPrepareMemory(void) {
    mock.prepares++;
    return mock.prepared;
}

// 每次 self 查询均产生独立发送权，与快照内当前线程的发送权分别计数。
mach_port_t mach_thread_self(void) {
    mock.selfCalls++;
    mock.threads[CURRENT_THREAD].references++;
    return CURRENT_THREAD;
}

// 模拟可随轮次变化的线程列表，并为每个成员增加一次快照发送权。
kern_return_t task_threads(task_t task, thread_act_array_t *threads, mach_msg_type_number_t *count) {
    REQUIRE(task == mach_task_self());
    mock.enumerations++;
    if (mock.enumerations == mock.enumerationFailure) return KERN_FAILURE;
    unsigned index = mock.enumerations - 1;
    if (index >= mock.snapshotCount) index = mock.snapshotCount - 1;
    const struct MockSnapshot *snapshot = &mock.snapshots[index];
    *count = snapshot->count;
    *threads = *count ? recordAllocation(*count * sizeof(thread_t), false) : NULL;
    for (unsigned i = 0; i < *count; i++) {
        thread_t thread = snapshot->threads[i];
        REQUIRE(thread > 0 && thread < MAX_THREADS);
        (*threads)[i] = thread;
        mock.threads[thread].references++;
    }
    return KERN_SUCCESS;
}

// 仅记录一次事务所拥有的暂停，保留线程原有暂停深度。
kern_return_t thread_suspend(thread_act_t thread) {
    REQUIRE(thread > CURRENT_THREAD && thread < MAX_THREADS);
    struct MockThread *state = &mock.threads[thread];
    REQUIRE(state->references && !state->ownedSuspension);
    state->suspends++;
    if (state->suspendResult == KERN_SUCCESS) {
        state->ownedSuspension = true;
        state->suspendDepth++;
    }
    return state->suspendResult;
}

// 当前实现只预热当前线程的状态读取，不修改被暂停线程的寄存器。
kern_return_t thread_get_state(thread_act_t thread, thread_state_flavor_t flavor,
        thread_state_t output, mach_msg_type_number_t *count) {
    REQUIRE(thread == CURRENT_THREAD && flavor == ARM_THREAD_STATE64);
    REQUIRE(*count == ARM_THREAD_STATE64_COUNT);
    mock.threads[thread].reads++;
    memset(output, 0, sizeof(arm_thread_state64_t));
    return KERN_SUCCESS;
}

// 旧 PC 修复路径不得重新进入本次暂停实现。
kern_return_t thread_set_state(thread_act_t thread, thread_state_flavor_t flavor,
        thread_state_t input, mach_msg_type_number_t count) {
    (void)thread; (void)flavor; (void)input; (void)count;
    REQUIRE(false);
    return KERN_FAILURE;
}

// 空端口仅用于预热，真实恢复只撤销本事务成功增加的暂停计数。
kern_return_t thread_resume(thread_act_t thread) {
    if (thread == MACH_PORT_NULL) {
        mock.nullResumes++;
        return KERN_INVALID_ARGUMENT;
    }
    REQUIRE(thread > CURRENT_THREAD && thread < MAX_THREADS);
    struct MockThread *state = &mock.threads[thread];
    REQUIRE(state->ownedSuspension && state->suspendDepth > state->initialDepth);
    state->ownedSuspension = false;
    state->suspendDepth--;
    state->resumes++;
    return KERN_SUCCESS;
}

// 重复快照可释放多余发送权，但最后一个发送权必须在恢复之后释放。
kern_return_t mach_port_deallocate(ipc_space_t task, mach_port_name_t thread) {
    REQUIRE(task == mach_task_self() && thread > 0 && thread < MAX_THREADS);
    struct MockThread *state = &mock.threads[thread];
    REQUIRE(state->references > 0);
    if (state->references == 1) REQUIRE(!state->ownedSuspension);
    state->references--;
    state->releases++;
    return KERN_SUCCESS;
}

// 累计数组只经 VM 分配，允许注入首次扩容失败。
kern_return_t vm_allocate(vm_map_t task, vm_address_t *address, vm_size_t size, int flags) {
    REQUIRE(task == mach_task_self() && flags == VM_FLAGS_ANYWHERE);
    REQUIRE(size > 0 && size % vm_page_size == 0);
    mock.vmAllocations++;
    if (mock.vmAllocations == mock.allocationFailure) return KERN_RESOURCE_SHORTAGE;
    *address = (vm_address_t)recordAllocation(size, true);
    return KERN_SUCCESS;
}

// 精确匹配原始分配长度，拒绝重复释放或把快照长度误用于累计数组。
kern_return_t vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size) {
    REQUIRE(task == mach_task_self());
    if (!address && !size) {
        mock.zeroDeallocations++;
        return KERN_SUCCESS;
    }
    for (unsigned i = 0; i < MAX_ALLOCATIONS; i++) {
        struct MockAllocation *entry = &mock.allocations[i];
        if (entry->address != (void *)address) continue;
        REQUIRE(entry->size == size);
        if (entry->cumulative) mock.listReleases++;
        else mock.snapshotReleases++;
        free(entry->address);
        *entry = (struct MockAllocation){0};
        return KERN_SUCCESS;
    }
    REQUIRE(false);
    return KERN_INVALID_ADDRESS;
}

// 成功或失败结束后，重复恢复均不再访问已经清空的资源。
static void requireReleased(struct LHThreadPause *pause) {
    REQUIRE(!pause->threads && !pause->count && !pause->allocationSize);
    requireClean();
    unsigned released = mock.listReleases + mock.snapshotReleases;
    LHResumeThreads(pause);
    REQUIRE(mock.listReleases + mock.snapshotReleases == released);
    requireClean();
}

// 准备、首次枚举和累计数组分配失败均应保持未暂停状态。
static void testPreparationFailures(void) {
    struct LHThreadPause pause;
    resetFixture();
    mock.prepared = false;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.prepares == 1 && !mock.selfCalls && !mock.enumerations);
    requireReleased(&pause);
    resetFixture();
    mock.enumerationFailure = 1;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 1 && !mock.vmAllocations);
    requireReleased(&pause);
    resetFixture();
    mock.allocationFailure = 1;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 1 && mock.snapshotReleases == 1);
    REQUIRE(!mock.threads[2].suspends && !mock.threads[3].suspends);
    requireReleased(&pause);
}

// 稳定列表只暂停一次；当前线程和此前已存在的暂停深度均不受影响。
static void testStableThreads(void) {
    resetFixture();
    mock.threads[3].initialDepth = mock.threads[3].suspendDepth = 2;
    struct LHThreadPause pause;
    REQUIRE(LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 2 && pause.count == 3);
    REQUIRE(!mock.threads[CURRENT_THREAD].suspends);
    REQUIRE(mock.threads[CURRENT_THREAD].reads == 1);
    for (unsigned i = 2; i <= 4; i++) {
        REQUIRE(mock.threads[i].suspends == 1 && mock.threads[i].ownedSuspension);
        REQUIRE(mock.threads[i].references == 1 && !mock.threads[i].reads);
    }
    LHResumeThreads(&pause);
    REQUIRE(mock.snapshotReleases == 2 && mock.listReleases == 1);
    requireReleased(&pause);
}

// 无其他线程时无需分配累计数组，仍须释放当前线程的所有发送权。
static void testCurrentOnly(void) {
    resetFixture();
    const thread_t threads[] = {CURRENT_THREAD};
    setSnapshot(0, threads, 1);
    struct LHThreadPause pause;
    REQUIRE(LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 1 && !mock.vmAllocations && !pause.count);
    LHResumeThreads(&pause);
    requireReleased(&pause);
}

// 后续快照出现的新线程逐个补齐，重复出现的旧线程不重复暂停。
static void testNewThreads(void) {
    resetFixture();
    const thread_t threads[] = {CURRENT_THREAD, 2, 3, 4};
    for (unsigned round = 0; round < 3; round++) setSnapshot(round, threads, round + 2);
    struct LHThreadPause pause;
    REQUIRE(LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 4 && pause.count == 3);
    for (unsigned i = 2; i <= 4; i++) REQUIRE(mock.threads[i].suspends == 1);
    LHResumeThreads(&pause);
    requireReleased(&pause);
}

// 中途暂停或后续枚举失败时，恢复此前已暂停线程并释放所有快照引用。
static void testMidwayFailures(void) {
    struct LHThreadPause pause;
    resetFixture();
    mock.threads[3].suspendResult = KERN_FAILURE;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.threads[2].suspends == 1 && mock.threads[2].resumes == 1);
    REQUIRE(mock.threads[3].suspends == 1 && !mock.threads[3].resumes);
    REQUIRE(!mock.threads[4].suspends);
    requireReleased(&pause);
    resetFixture();
    mock.enumerationFailure = 2;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 2);
    for (unsigned i = 2; i <= 4; i++) REQUIRE(mock.threads[i].resumes == 1);
    requireReleased(&pause);
}

// 枚举后已终止的线程不要求恢复；后续快照移除它后可以正常完成。
static void testTerminatedThreads(void) {
    const kern_return_t results[] = {KERN_TERMINATED, KERN_INVALID_ARGUMENT, MACH_SEND_INVALID_DEST};
    for (unsigned result = 0; result < sizeof(results) / sizeof(*results); result++) {
        resetFixture();
        const thread_t first[] = {CURRENT_THREAD, 2, 3};
        const thread_t second[] = {CURRENT_THREAD, 3};
        setSnapshot(0, first, 3);
        setSnapshot(1, second, 2);
        mock.threads[2].suspendResult = results[result];
        struct LHThreadPause pause;
        REQUIRE(LHPauseThreads(&pause));
        REQUIRE(mock.enumerations == 2 && pause.count == 1);
        REQUIRE(mock.threads[2].suspends == 1 && !mock.threads[2].resumes);
        REQUIRE(mock.threads[3].suspends == 1);
        LHResumeThreads(&pause);
        requireReleased(&pause);
    }
}

// 持续出现新线程时达到轮数上限即失败，不将不完整暂停报告为成功。
static void testSnapshotLimit(void) {
    resetFixture();
    thread_t threads[10];
    for (unsigned i = 0; i < 10; i++) threads[i] = i + 1;
    for (unsigned round = 0; round < 9; round++) setSnapshot(round, threads, round + 2);
    struct LHThreadPause pause;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 8);
    for (unsigned i = 2; i <= 9; i++)
        REQUIRE(mock.threads[i].suspends == 1 && mock.threads[i].resumes == 1);
    REQUIRE(!mock.threads[10].suspends);
    requireReleased(&pause);
}

// 不消失的死亡线程也应有界退出，避免无限重试持有已经暂停的其他线程。
static void testPersistentTerminatedThread(void) {
    resetFixture();
    mock.threads[2].suspendResult = KERN_TERMINATED;
    struct LHThreadPause pause;
    REQUIRE(!LHPauseThreads(&pause));
    REQUIRE(mock.enumerations == 8 && mock.threads[2].suspends == 8);
    REQUIRE(mock.threads[3].suspends == 1 && mock.threads[3].resumes == 1);
    requireReleased(&pause);
}

// 所有内核入口均被桩替代，验证当前暂停语义而不依赖旧 PC 修复算法。
int main(void) {
    testPreparationFailures();
    testStableThreads();
    testCurrentOnly();
    testNewThreads();
    testMidwayFailures();
    testTerminatedThreads();
    testSnapshotLimit();
    testPersistentTerminatedThread();
    requireClean();
    printf("PASS thread snapshots, failure cleanup and send rights: %u checks\n", checks);
    return 0;
}
