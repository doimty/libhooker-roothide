//
//  shadowMem.c
//  libhooker-test
//
//  Created by CoolStar on 9/4/20.
//  Copyright © 2020 CoolStar. All rights reserved.
//

#include "funcMacros.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "shadowMem.h"
#define LIBHOOKER_PRIVATE 1
#include "libhooker.h"

// 沿用上游分配式读取接口，并检查跨页边界和内核读取结果。
void *LHReadShadowMem(void *ptr, size_t size) {
    if (!ptr || !size || size > UINTPTR_MAX - (uintptr_t)ptr) return NULL;
    uint8_t *buffer = malloc(size);
    if (!buffer) return NULL;
    size_t copied = 0;
    while (copied < size) {
        uintptr_t address = (uintptr_t)ptr + copied;
        size_t offset = address & PAGE_MASK;
        size_t amount = PAGE_SIZE - offset;
        if (amount > size - copied) amount = size - copied;
        if (!LHCopyMemPage(buffer + copied, address & ~PAGE_MASK, offset, amount)) {
            free(buffer);
            return NULL;
        }
        copied += amount;
    }
    return buffer;
}

// 先检查全部目标页，再写入影子副本，避免跨页失败留下半个补丁。
bool LHWriteMemoryInternal(void *destination, const void *data, size_t size){
    if (!destination || !data || !size || size > UINTPTR_MAX - (uintptr_t)destination) return false;
    LHLockMemory();
    for (int pass = 0; pass < 2; pass++) {
        size_t totalBytes = 0;
        while (totalBytes < size) {
            uintptr_t ptr = (uintptr_t)destination + totalBytes;
            uintptr_t page_base = ptr & ~PAGE_MASK;
            size_t offset = ptr - page_base;
            size_t bufSiz = PAGE_SIZE - offset;
            if (size - totalBytes < bufSiz) bufSiz = size - totalBytes;
            if (!pass) {
                if (!LHPrepareShadowPage(page_base)) {
                    LHUnlockMemory();
                    return false;
                }
            } else {
                // 所有页已在同一把锁下准备完毕，此处不会再读取或分配目标页。
                if (!LHWriteShadowPage(page_base, offset, (const uint8_t *)data + totalBytes, bufSiz)) {
                    LHUnlockMemory();
                    return false;
                }
            }
            totalBytes += bufSiz;
        }
    }
    LHUnlockMemory();
    return true;
}

// 在同一事务内提交整批内存补丁。
LIBHOOKER_EXPORT int LHPatchMemory(const struct LHMemoryPatch *patches, int count){
    if (!patches || count < 0) { errno = LIBHOOKER_ERR_VM; return 0; }
    LHLockMemory();
    int successCount = 0;
    errno = 0;
    for (int i = 0; i < count; i++){
        if (LHWriteMemoryInternal(patches[i].destination, patches[i].data, patches[i].size))
            successCount++;
        else
            errno = LIBHOOKER_ERR_VM;
    }
    if (!LHCommitShadowPages()) {
        successCount = 0;
    }
    int commitError = errno;
    LHUnlockMemory();
    errno = commitError;
    return successCount;
}

// 兼容单个补丁的内部入口。
LIBHOOKER_EXPORT bool LHWriteMemory(void *destination, const void *data, size_t size) {
    struct LHMemoryPatch patch[] = {
        {destination, data, size, NULL}
    };
    return LHPatchMemory(patch, 1) == 1;
}
