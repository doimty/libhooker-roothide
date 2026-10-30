#ifndef LH_THREAD_SAFETY_H
#define LH_THREAD_SAFETY_H
#include <stdbool.h>
#include <stdint.h>
#include <mach/mach.h>
#ifdef __cplusplus
extern "C" {
#endif
struct LHThreadPause {
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    // 累计列表按 VM 页分配，恢复时释放完整容量。
    vm_size_t allocationSize;
};
bool LHPrepareMemory(void);
bool LHPauseThreads(struct LHThreadPause *pause);
void LHResumeThreads(struct LHThreadPause *pause);
#ifdef __cplusplus
}
#endif
#endif
