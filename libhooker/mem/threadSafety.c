// 提交内存补丁前补齐枚举期间新建的线程；不改写线程寄存器或返回地址。
#include "threadSafety.h"
#include <mach/arm/thread_status.h>

#define LH_THREAD_SNAPSHOT_LIMIT 8

// 持有的发送权保持端口名称稳定，后续快照中的同一线程不重复暂停。
static bool LHThreadAlreadyPaused(const struct LHThreadPause *pause, thread_t thread) {
    for (mach_msg_type_number_t i = 0; i < pause->count; i++)
        if (pause->threads[i] == thread) return true;
    return false;
}

// 只用 VM 接口扩容，逐项复制避免暂停期间进入堆分配器或首次调用 memcpy。
static bool LHReserveThreadList(struct LHThreadPause *pause, mach_msg_type_number_t count) {
    if ((size_t)count > SIZE_MAX / sizeof(thread_t)) return false;
    size_t required = (size_t)count * sizeof(thread_t);
    if (required <= pause->allocationSize) return true;
    size_t pageSize = vm_page_size;
    if (!pageSize || required > SIZE_MAX - (pageSize - 1)) return false;
    vm_size_t allocationSize = (required + pageSize - 1) / pageSize * pageSize;
    vm_address_t allocation = 0;
    if (vm_allocate(mach_task_self(), &allocation, allocationSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
    volatile thread_t *destination = (volatile thread_t *)allocation;
    const volatile thread_t *source = pause->threads;
    for (mach_msg_type_number_t i = 0; i < pause->count; i++) destination[i] = source[i];
    if (pause->threads) vm_deallocate(mach_task_self(), (vm_address_t)pause->threads, pause->allocationSize);
    pause->threads = (thread_act_array_t)allocation;
    pause->allocationSize = allocationSize;
    return true;
}

// 释放本轮未转交给累计列表的发送权，以及内核分配的快照数组。
static void LHReleaseThreadSnapshot(thread_act_array_t threads, mach_msg_type_number_t count) {
    for (mach_msg_type_number_t i = 0; i < count; i++)
        if (threads[i] != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), threads[i]);
    if (threads) vm_deallocate(mach_task_self(), (vm_address_t)threads, (vm_size_t)count * sizeof(thread_t));
}

// 暂停后继续枚举，直到没有新线程；达到上限或任何非终止错误时完整恢复。
bool LHPauseThreads(struct LHThreadPause *pause) {
    pause->threads = NULL;
    pause->count = 0;
    pause->allocationSize = 0;
    if (!LHPrepareMemory()) return false;
    thread_t current = mach_thread_self();
    // 多指令入口预检会调用此接口，首次解析必须发生在线程暂停之前。
    arm_thread_state64_t state;
    mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
    thread_get_state(current, ARM_THREAD_STATE64, (thread_state_t)&state, &stateCount);
    // 恢复的首次解析也不能留到暂停区；空端口只返回错误，不改变任何线程。
    thread_resume(MACH_PORT_NULL);
    // 后续快照会在暂停区释放，先解析这两个释放接口；零长度 VM 释放不改动映射。
    mach_port_deallocate(mach_task_self(), mach_thread_self());
    vm_deallocate(mach_task_self(), 0, 0);
    bool success = false;
    for (unsigned round = 0; round < LH_THREAD_SNAPSHOT_LIMIT; round++) {
        thread_act_array_t snapshot = NULL;
        mach_msg_type_number_t count = 0;
        kern_return_t kr = task_threads(mach_task_self(), &snapshot, &count);
        if (kr != KERN_SUCCESS) break;
        mach_msg_type_number_t newCount = 0;
        for (mach_msg_type_number_t i = 0; i < count; i++)
            if (snapshot[i] != current && !LHThreadAlreadyPaused(pause, snapshot[i])) newCount++;
        if (!newCount) {
            LHReleaseThreadSnapshot(snapshot, count);
            success = true;
            break;
        }
        if (newCount > UINT32_MAX - pause->count || !LHReserveThreadList(pause, pause->count + newCount)) {
            LHReleaseThreadSnapshot(snapshot, count);
            break;
        }
        bool failed = false;
        for (mach_msg_type_number_t i = 0; i < count; i++) {
            thread_t thread = snapshot[i];
            if (thread == current || LHThreadAlreadyPaused(pause, thread)) continue;
            kr = thread_suspend(thread);
            if (kr == KERN_SUCCESS) {
                pause->threads[pause->count++] = thread;
                snapshot[i] = MACH_PORT_NULL; // 发送权随线程转入累计列表。
            } else if (kr != KERN_TERMINATED && kr != KERN_INVALID_ARGUMENT && kr != MACH_SEND_INVALID_DEST) {
                failed = true;
                break;
            }
        }
        LHReleaseThreadSnapshot(snapshot, count);
        if (failed) break;
        // 即使新线程已终止也要重查，它可能在终止之前创建了未列入快照的子线程。
    }
    mach_port_deallocate(mach_task_self(), current);
    if (!success) LHResumeThreads(pause);
    return success;
}

// 每个成功暂停的线程只恢复一次，再释放所有发送权和累计列表。
void LHResumeThreads(struct LHThreadPause *pause) {
    for (mach_msg_type_number_t i = 0; i < pause->count; i++) {
        if (pause->threads[i] != MACH_PORT_NULL) thread_resume(pause->threads[i]);
    }
    for (mach_msg_type_number_t i = 0; i < pause->count; i++) {
        if (pause->threads[i] != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), pause->threads[i]);
    }
    if (pause->threads) vm_deallocate(mach_task_self(), (vm_address_t)pause->threads, pause->allocationSize);
    pause->threads = NULL;
    pause->count = 0;
    pause->allocationSize = 0;
}
