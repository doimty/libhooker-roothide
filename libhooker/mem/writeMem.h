//
//  writeMem.h
//  libhooker
//
//  Created by CoolStar on 2/12/20.
//  Copyright © 2020 CoolStar. All rights reserved.
//

#ifndef writeMem_h
#define writeMem_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool LHMarkMemoryWriteable(void *data);
bool LHMarkMemoryExecutable(void *data);
bool LHExecMemory(void **page, void *data, size_t size);
bool LHWriteMemoryInternal(void *destination, const void *data, size_t size);

#ifdef LH_ROOTHIDE
__attribute__((visibility("hidden"))) bool LHGetMemoryProtection(uintptr_t page, uint32_t *protection);
__attribute__((visibility("hidden"))) bool LHCommitMemoryRanges(uintptr_t page, const void *data,
    size_t size, const uint8_t *mask, uint32_t protection);
__attribute__((visibility("hidden"))) bool LHRestoreMemoryRanges(uintptr_t page, const void *original,
    size_t size, const uint8_t *mask, uint32_t protection, void *scratch);
#endif

#ifdef __cplusplus
}
#endif

#endif /* writeMem_h */
