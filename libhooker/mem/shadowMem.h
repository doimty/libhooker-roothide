//
//  writeMemQueue.hpp
//  libhooker
//
//  Created by CoolStar on 9/4/20.
//  Copyright © 2020 CoolStar. All rights reserved.
//

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef writeMemQueue_hpp
#define writeMemQueue_hpp

#ifdef __APPLE__
#include <mach/vm_param.h>
#else
extern size_t PAGE_SIZE;
extern size_t PAGE_MASK;
#endif

#ifdef __cplusplus
extern "C" {
#endif

void LHLockMemory(void);
void LHUnlockMemory(void);

void *LHReadShadowMem(void *ptr, size_t size);
bool LHCopyMemPage(void *destination, uintptr_t page, uintptr_t offset, size_t size);
bool LHPrepareShadowPage(uintptr_t page);
bool LHWriteShadowPage(uintptr_t page, size_t offset, const void *data, size_t size);
bool LHShadowRangeIsDirty(uintptr_t address, size_t size);
bool LHCommitShadowPages(void);

#ifdef __cplusplus
}
#endif

#endif /* writeMemQueue_hpp */
