#ifndef LH_CRASH_GUARD_H
#define LH_CRASH_GUARD_H
#include <stdbool.h>
bool LHInstallCrashGuard(const char *path);
bool LHMarkSafeMode(const char *path);
bool LHCrashGuardIsReady(void);
bool LHRequestSafeMode(const char *path);
bool LHIsSafeModeRequested(void);
int LHGetSafeModePersistenceError(void);
#endif
