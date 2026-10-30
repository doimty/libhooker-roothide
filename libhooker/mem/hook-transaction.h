#ifndef LH_HOOK_TRANSACTION_H
#define LH_HOOK_TRANSACTION_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct LHHookResources {
    void **oldptr;
    void *previousOriginal;
    void *original;
    void *trampoline;
    void *relay;
    uintptr_t target;
    size_t patchSize;
    struct LHHookResources *next;
};

struct LHThreadPause;
void LHQueueHookResources(struct LHHookResources *resources);
bool LHValidateHookResources(const struct LHThreadPause *pause);
bool LHPrepareHookPointers(void);
void LHPublishHookPointers(void);
bool LHRestoreHookPointers(void);
void LHFinishHookResources(bool preserve);

#ifdef __cplusplus
}
#endif
#endif
