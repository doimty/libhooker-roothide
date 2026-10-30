// 崩溃标记与 UIKit 分离，便于在隔离进程中验证恢复流程。
#include "crash-guard.h"
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static char markerPath[PATH_MAX];
static const int crashSignals[] = {SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGEMT, SIGFPE, SIGBUS, SIGSEGV, SIGSYS};
static bool crashGuardReady;
static bool safeModeRequested;
static int safeModePersistenceError;

// 仅使用异步信号安全的文件操作记录故障。
bool LHMarkSafeMode(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    return close(fd) == 0;
}

// 主动请求先保留进程内状态，磁盘不可写也不能继续加载故障插件。
bool LHRequestSafeMode(const char *path) {
    safeModeRequested = true;
    if (!path || !*path) {
        safeModePersistenceError = EINVAL;
        return false;
    }
    bool persisted = LHMarkSafeMode(path);
    safeModePersistenceError = persisted ? 0 : errno;
    return persisted;
}

// 供加载器读取构造阶段已确定的主动安全模式请求。
bool LHIsSafeModeRequested(void) { return safeModeRequested; }

// 只向界面传递主动请求的持久化错误，不在信号处理器中显示提示。
int LHGetSafeModePersistenceError(void) { return safeModePersistenceError; }

// Darwin 不会为 SIGILL/SIGTRAP 执行 SA_RESETHAND；所有信号统一显式恢复默认动作。
// sigaction、sigemptyset、open、close、getpid、kill、_exit 均可在信号处理器中使用。
static void LHHandleCrash(int signalNumber) {
    struct sigaction action = {0};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    // 恢复失败不能继续重发并反复进入处理器，退出码也不得伪装成 128 + signal。
    if (sigaction(signalNumber, &action, NULL) != 0) _exit(127);
    LHMarkSafeMode(markerPath);
    if (kill(getpid(), signalNumber) != 0) _exit(127);
}

// 在插件初始化之前注册处理器，路径数据在收到信号前准备完毕。
bool LHInstallCrashGuard(const char *path) {
    if (!path || !*path) return false;
    size_t length = strlen(path);
    if (length >= sizeof(markerPath)) return false;
    // 注册成功后路径保持只读，避免信号处理器观察到部分更新的路径。
    if (crashGuardReady) return strcmp(path, markerPath) == 0;
    memcpy(markerPath, path, length + 1);
    struct sigaction action = {0};
    action.sa_handler = LHHandleCrash;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    struct sigaction previous[sizeof(crashSignals) / sizeof(*crashSignals)];
    for (unsigned i = 0; i < sizeof(crashSignals) / sizeof(*crashSignals); i++) {
        if (sigaction(crashSignals[i], &action, &previous[i]) != 0) {
            // 部分注册失败时恢复原处理器，避免留下半初始化的保护状态。
            while (i > 0) { i--; sigaction(crashSignals[i], &previous[i], NULL); }
            markerPath[0] = '\0';
            return false;
        }
    }
    crashGuardReady = true;
    return true;
}

// 供加载器确认保护已经就绪，库能打开本身并不代表注册成功。
bool LHCrashGuardIsReady(void) { return crashGuardReady; }
