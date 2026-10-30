//
//  writeMemQueue.cpp
//  libhooker
//
//  Created by CoolStar on 9/4/20.
//  Copyright © 2020 CoolStar. All rights reserved.
//

// 串行维护补丁事务及其影子页。
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <pthread.h>
#include <cerrno>
#include "shadowMem.h"
#include "libhooker.h"
#ifdef LH_ROOTHIDE
#include "threadSafety.h"
#include "hook-transaction.h"
#include "header-relay.h"
#include "writeMem.h"
#include <mach/mach_vm.h>
#endif

extern "C" bool LHCommitMemory(uintptr_t destination, const void *data, size_t size);
struct LHShadowPage {
    uint8_t *bytes;
    bool dirty;
#ifdef LH_ROOTHIDE
    uint8_t *original;
    uint32_t protection;
    bool targetAttempted;
    bool relayAttempted;
#endif
};
static std::map<uintptr_t, LHShadowPage> *shadowPages;
static pthread_mutex_t shadowPagesLock;
static pthread_once_t once = PTHREAD_ONCE_INIT;

// 创建递归锁，允许批量 Hook 内部复用读写接口。
static void LHInitializeShadowPages(void) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&shadowPagesLock, &attributes);
    pthread_mutexattr_destroy(&attributes);
    shadowPages = new (std::nothrow) std::map<uintptr_t, LHShadowPage>;
}

// 开始一次完整的补丁事务。
extern "C" void LHLockMemory(void) {
    pthread_once(&once, LHInitializeShadowPages);
    pthread_mutex_lock(&shadowPagesLock);
}

// 结束当前补丁事务。
extern "C" void LHUnlockMemory(void) {
    pthread_mutex_unlock(&shadowPagesLock);
}

// 通过统一入口读取完整页，提交前刷新也必须检查内核报告的实际长度。
static bool LHReadPage(uintptr_t page, void *buffer) {
#ifdef LH_ROOTHIDE
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), page, PAGE_SIZE,
        (mach_vm_address_t)buffer, &copied) == KERN_SUCCESS && copied == PAGE_SIZE;
#else
    memcpy(buffer, (const void *)page, PAGE_SIZE);
    return true;
#endif
}

// 在持锁状态下复用整页快照，位图与数据共用一次分配，读取不标记待写字节。
static LHShadowPage *LHFindOrReadShadowPage(uintptr_t page) {
    if (page & PAGE_MASK) return NULL;
    if (!shadowPages) {
        shadowPages = new (std::nothrow) std::map<uintptr_t, LHShadowPage>;
        if (!shadowPages) return NULL;
    }
    auto entry = shadowPages->find(page);
    if (entry != shadowPages->end()) return &entry->second;
    uint8_t *buffer = (uint8_t *)malloc(PAGE_SIZE + PAGE_SIZE / 8);
    if (!buffer) return NULL;
    if (!LHReadPage(page, buffer)) { free(buffer); return NULL; }
    memset(buffer + PAGE_SIZE, 0, PAGE_SIZE / 8);
    try {
        LHShadowPage pageState = {};
        pageState.bytes = buffer;
        auto inserted = shadowPages->insert(std::make_pair(page, pageState));
        if (!inserted.second) free(buffer);
        return &inserted.first->second;
    } catch (const std::bad_alloc &) {
        free(buffer);
        return NULL;
    }
}

// 预先取得可读快照，供跨页写在修改任何字节前完成全部页的检查。
extern "C" bool LHPrepareShadowPage(uintptr_t page) {
    LHLockMemory();
    bool success = LHFindOrReadShadowPage(page) != NULL;
    LHUnlockMemory();
    return success;
}

// 标记精确字节范围，整组八字节使用批量置位以减少大补丁的逐字节开销。
static void LHMarkShadowRange(uint8_t *mask, size_t offset, size_t size) {
    size_t end = offset + size;
    while (offset < end && (offset & 7)) {
        mask[offset / 8] |= (uint8_t)(1u << (offset & 7));
        offset++;
    }
    size_t groups = (end - offset) / 8;
    memset(mask + offset / 8, 0xff, groups);
    offset += groups * 8;
    while (offset < end) {
        mask[offset / 8] |= (uint8_t)(1u << (offset & 7));
        offset++;
    }
}

// 仅修改已准备的快照并记录目标范围，同一事务的后续读取立即看见补丁。
extern "C" bool LHWriteShadowPage(uintptr_t page, size_t offset, const void *data, size_t size) {
    if (!data || !size || (page & PAGE_MASK) || offset > PAGE_SIZE || size > PAGE_SIZE - offset) return false;
    LHLockMemory();
    bool success = false;
    if (shadowPages) {
        auto entry = shadowPages->find(page);
        if (entry != shadowPages->end()) {
            memcpy(entry->second.bytes + offset, data, size);
            LHMarkShadowRange(entry->second.bytes + PAGE_SIZE, offset, size);
            entry->second.dirty = true;
            success = true;
        }
    }
    LHUnlockMemory();
    return success;
}

// 调用者已持有事务锁；每页只查一次快照，输出槽不能与补丁字节相交。
extern "C" bool LHShadowRangeIsDirty(uintptr_t address, size_t size) {
    if (size > UINTPTR_MAX - address) return true;
    if (!shadowPages) return false;
    while (size) {
        size_t index = address & PAGE_MASK;
        size_t amount = PAGE_SIZE - index;
        if (amount > size) amount = size;
        auto entry = shadowPages->find(address & ~PAGE_MASK);
        if (entry != shadowPages->end() && entry->second.dirty) {
            const uint8_t *mask = entry->second.bytes + PAGE_SIZE;
            for (size_t end = index + amount; index < end; index++)
                if (mask[index / 8] & (1u << (index & 7))) return true;
        }
        address += amount;
        size -= amount;
    }
    return false;
}

// 暂停后保存撤销快照；传统平台仍在这里合并实时非目标字节。
static bool LHRefreshShadowPage(uintptr_t page, LHShadowPage &entry, uint8_t *scratch) {
    if (!LHReadPage(page, scratch)) return false;
#ifdef LH_ROOTHIDE
    if (!LHGetMemoryProtection(page, &entry.protection)) return false;
    memcpy(entry.original, scratch, PAGE_SIZE);
#else
    const uint8_t *mask = entry.bytes + PAGE_SIZE;
    for (size_t group = 0; group < PAGE_SIZE / 8; group++) {
        uint8_t bits = mask[group];
        size_t offset = group * 8;
        if (!bits) {
            memcpy(entry.bytes + offset, scratch + offset, 8);
        } else if (bits != 0xff) {
            for (unsigned bit = 0; bit < 8; bit++)
                if (!(bits & (1u << bit))) entry.bytes[offset + bit] = scratch[offset + bit];
        }
    }
#endif
    return true;
}

#ifdef LH_ROOTHIDE
// 仅提交本阶段字节，实时合并同页其他内容；先备齐中继，再发布原调用和入口。
static bool LHSubmitShadowPages(uint8_t *scratch, bool relays) {
    for (auto &entry : *shadowPages) {
        LHShadowPage &page = entry.second;
        if (!page.dirty) continue;
        const uint8_t *mask = page.original + PAGE_SIZE + (relays ? PAGE_SIZE / 8 : 0);
        bool hasBytes = false;
        for (size_t group = 0; group < PAGE_SIZE / 8; group++) hasBytes |= mask[group] != 0;
        if (!hasBytes) continue;
        if (!LHReadPage(entry.first, scratch)) return false;
        for (size_t group = 0; group < PAGE_SIZE / 8; group++) {
            uint8_t bits = mask[group];
            size_t offset = group * 8;
            if (bits == 0xff) memcpy(scratch + offset, page.bytes + offset, 8);
            else if (bits)
                for (unsigned bit = 0; bit < 8; bit++)
                    if (bits & (1u << bit)) scratch[offset + bit] = page.bytes[offset + bit];
        }
        // 后端错误也可能已部分改写，必须在调用前将该阶段纳入撤销。
        if (relays) page.relayAttempted = true;
        else page.targetAttempted = true;
        if (!LHCommitMemoryRanges(entry.first, scratch, PAGE_SIZE, mask, page.protection)) return false;
    }
    return true;
}

// 分阶段恢复入口与中继；oldptr 尚未全部还原前，不撤销它可能依赖的中继字节。
static bool LHRollbackShadowPages(uint8_t *scratch, bool relays) {
    for (auto entry = shadowPages->rbegin(); entry != shadowPages->rend(); ++entry) {
        LHShadowPage &page = entry->second;
        if (!(relays ? page.relayAttempted : page.targetAttempted)) continue;
        const uint8_t *mask = page.original + PAGE_SIZE + (relays ? PAGE_SIZE / 8 : 0);
        bool hasBytes = false;
        for (size_t group = 0; group < PAGE_SIZE / 8; group++) hasBytes |= mask[group] != 0;
        if (!hasBytes) continue;
        if (!LHRestoreMemoryRanges(entry->first, page.original, PAGE_SIZE,
                mask, page.protection, scratch)) return false;
    }
    return true;
}
#endif

// 提交失败时先恢复已经尝试的页面，无法确认恢复时显式返回回滚错误。
extern "C" bool LHCommitShadowPages(void) {
    LHLockMemory();
    int savedError = errno;
    bool hasDirtyPages = false;
    if (shadowPages)
        for (const auto &entry : *shadowPages) hasDirtyPages |= entry.second.dirty;
    // scratch 必须在线程暂停前分配，所有释放均在恢复之后。
#ifdef LH_ROOTHIDE
    size_t scratchSize = PAGE_SIZE * 2;
#else
    size_t scratchSize = PAGE_SIZE;
#endif
    uint8_t *scratch = hasDirtyPages ? (uint8_t *)malloc(scratchSize) : NULL;
    bool success = !hasDirtyPages || scratch;
#ifdef LH_ROOTHIDE
    // 每页撤销副本必须在暂停前准备，暂停期间只读取原内容及权限。
    if (success && hasDirtyPages) {
        for (auto &entry : *shadowPages) {
            if (!entry.second.dirty) continue;
            entry.second.original = (uint8_t *)malloc(PAGE_SIZE + PAGE_SIZE / 4);
            if (!entry.second.original) { success = false; break; }
            uint8_t *targetMask = entry.second.original + PAGE_SIZE;
            uint8_t *relayMask = targetMask + PAGE_SIZE / 8;
            LHGetPendingHeaderRelayMask(entry.first, relayMask, PAGE_SIZE);
            const uint8_t *dirtyMask = entry.second.bytes + PAGE_SIZE;
            for (size_t group = 0; group < PAGE_SIZE / 8; group++) {
                targetMask[group] = dirtyMask[group] & (uint8_t)~relayMask[group];
                relayMask[group] &= dirtyMask[group];
            }
        }
    }
    struct LHThreadPause pause = {};
    bool preserveResources = false;
    bool pointersPublished = false;
    bool rollbackFailed = false;
    if (success && hasDirtyPages) success = LHPauseThreads(&pause);
#endif
    // 先刷新全部脏页；任一读取失败时，尚未向底层提交任何页面。
    if (success && hasDirtyPages) {
        for (auto &entry : *shadowPages) {
            if (entry.second.dirty && !LHRefreshShadowPage(entry.first, entry.second, scratch)) {
                success = false;
                break;
            }
        }
    }
#ifdef LH_ROOTHIDE
    if (success && hasDirtyPages) success = LHValidateHookResources(&pause) && LHPrepareHookPointers();
    // 任意已发布 original 都可能依赖本批其他页的中继，不能等到逐页入口提交时才写入。
    if (success && hasDirtyPages) success = LHSubmitShadowPages(scratch, true);
    if (success && hasDirtyPages) {
        LHPublishHookPointers();
        pointersPublished = true;
        success = LHSubmitShadowPages(scratch, false);
    }
#else
    if (success && hasDirtyPages) {
        for (auto &entry : *shadowPages) {
            if (!entry.second.dirty) continue;
            success = LHCommitMemory(entry.first, entry.second.bytes, PAGE_SIZE);
            if (!success) break;
        }
    }
#endif
#ifdef LH_ROOTHIDE
    if (!success && hasDirtyPages) {
        rollbackFailed = !LHRollbackShadowPages(scratch, false);
        if (!rollbackFailed && pointersPublished) rollbackFailed = !LHRestoreHookPointers();
        if (!rollbackFailed) rollbackFailed = !LHRollbackShadowPages(scratch, true);
    }
    preserveResources = success || rollbackFailed;
    LHResumeThreads(&pause);
    LHFinishHookResources(preserveResources);
#endif
    free(scratch);
    if (shadowPages) {
        for (const auto &entry : *shadowPages) {
            free(entry.second.bytes);
#ifdef LH_ROOTHIDE
            free(entry.second.original);
#endif
        }
        shadowPages->clear();
    }
    LHUnlockMemory();
    errno = success ? savedError : LIBHOOKER_ERR_VM;
#ifdef LH_ROOTHIDE
    if (rollbackFailed) errno = LIBHOOKER_ERR_ROLLBACK;
#endif
    return success;
}

// 优先读取当前事务中的待提交内容，以保留重复 Hook 的调用链。
extern "C" bool LHCopyMemPage(void *destination, uintptr_t page, uintptr_t offset, size_t size) {
    if (!destination || offset > PAGE_SIZE || size > PAGE_SIZE - offset) return false;
    LHLockMemory();
    LHShadowPage *entry = LHFindOrReadShadowPage(page);
    bool success = entry != NULL;
    if (success) memcpy(destination, (const uint8_t *)entry->bytes + offset, size);
    LHUnlockMemory();
    return success;
}
