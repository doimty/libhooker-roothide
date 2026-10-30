// 事务只管理独立跳板的生命周期与原调用发布，不合并跳板页。
#include "hook-transaction.h"
#include "header-relay.h"
#include "shadowMem.h"
#include "threadSafety.h"
#include "writeMem.h"
#include <mach/arm/thread_status.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>

static struct LHHookResources *pending;
static struct LHHookResources *pendingLast;

// 多指令补丁不得覆盖暂停线程的当前指令或可见返回位置。
bool LHValidateHookResources(const struct LHThreadPause *pause) {
    if (!LHValidateHeaderRelays()) return false;
    bool multiple = false;
    for (const struct LHHookResources *entry = pending; entry; entry = entry->next)
        multiple |= entry->patchSize > sizeof(uint32_t);
    if (!multiple) return true;
    for (mach_msg_type_number_t i = 0; i < pause->count; i++) {
        if (pause->threads[i] == MACH_PORT_NULL) continue;
        arm_thread_state64_t state;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        kern_return_t result = thread_get_state(pause->threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &count);
        if (result == KERN_TERMINATED || result == MACH_SEND_INVALID_DEST) continue;
        if (result != KERN_SUCCESS || count != ARM_THREAD_STATE64_COUNT) return false;
        uintptr_t pc = arm_thread_state64_get_pc(state), lr = arm_thread_state64_get_lr(state);
        for (const struct LHHookResources *entry = pending; entry; entry = entry->next) {
            if ((pc > entry->target && pc - entry->target < entry->patchSize) ||
                (lr > entry->target && lr - entry->target < entry->patchSize)) return false;
        }
    }
    return true;
}

// 保持同批安装的顺序，使重复 Hook 的原调用依次连接。
void LHQueueHookResources(struct LHHookResources *resources) {
    if (pendingLast) pendingLast->next = resources;
    else pending = resources;
    pendingLast = resources;
}

// 暂停后读取调用者输出槽，拒绝非对齐、不可写或与本次补丁相交的地址。
bool LHPrepareHookPointers(void) {
    for (struct LHHookResources *entry = pending; entry; entry = entry->next) {
        if (!entry->oldptr) continue;
        uintptr_t address = (uintptr_t)entry->oldptr;
        uint32_t protection;
        if ((address & (sizeof(void *) - 1)) || address > UINTPTR_MAX - sizeof(void *) ||
                LHShadowRangeIsDirty(address, sizeof(void *)) ||
                !LHGetMemoryProtection(address & ~PAGE_MASK, &protection) ||
                !(protection & VM_PROT_WRITE)) return false;
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), address, sizeof(void *),
                (mach_vm_address_t)&entry->previousOriginal, &copied) != KERN_SUCCESS ||
                copied != sizeof(void *)) return false;
    }
    return true;
}

// 暂停线程期间先发布原调用，再允许入口跳转到替换函数。
void LHPublishHookPointers(void) {
    for (struct LHHookResources *entry = pending; entry; entry = entry->next)
        if (entry->oldptr) *entry->oldptr = entry->original;
    atomic_thread_fence(memory_order_release);
}

// 入口全部恢复后还原输出槽，再允许撤销头部中继；重复槽位保存发布前同一原值。
bool LHRestoreHookPointers(void) {
    bool success = true;
    for (struct LHHookResources *entry = pending; entry; entry = entry->next) {
        if (!entry->oldptr) continue;
        uint32_t protection;
        uintptr_t address = (uintptr_t)entry->oldptr;
        if (!LHGetMemoryProtection(address & ~PAGE_MASK, &protection) ||
                !(protection & VM_PROT_WRITE)) { success = false; continue; }
        *entry->oldptr = entry->previousOriginal;
        void *restored = NULL;
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), address, sizeof(restored),
                (mach_vm_address_t)&restored, &copied) != KERN_SUCCESS ||
                copied != sizeof(restored) || restored != entry->previousOriginal) success = false;
    }
    atomic_thread_fence(memory_order_release);
    return success;
}

// 恢复线程后释放未发布或已确认回滚的分配，不确定的原调用和中继永久保留。
void LHFinishHookResources(bool preserve) {
    while (pending) {
        struct LHHookResources *next = pending->next;
        if (!preserve) {
            if (pending->trampoline) munmap(pending->trampoline, PAGE_SIZE);
            if (pending->relay) munmap(pending->relay, PAGE_SIZE);
        }
        free(pending);
        pending = next;
    }
    pendingLast = NULL;
    LHFinishHeaderRelays(preserve);
}
