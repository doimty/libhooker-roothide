// 只保存已加载镜像的路径，查询时持有独立副本并在锁外取得 dyld 引用。
#include "dyldSyms.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct LHImageRecord {
    const void *header;
    struct LHImageRecord *next;
    size_t pathSize;
    char path[];
};

static struct LHImageRecord *imageRecords;
static struct LHImageRecord **imageTail = &imageRecords;
static pthread_mutex_t imageLock = PTHREAD_MUTEX_INITIALIZER;

// dyld 通知期间复制路径，分配和 dyld 查询均在进入本地锁前完成。
static void LHImageAdded(const struct mach_header *header, intptr_t slide) {
    (void)slide;
    Dl_info info = {0};
    if (!dladdr(header, &info) || !info.dli_fname) return;
    size_t length = strlen(info.dli_fname);
    if (length > SIZE_MAX - sizeof(struct LHImageRecord) - 1) return;
    struct LHImageRecord *record = malloc(sizeof(*record) + length + 1);
    if (!record) return;
    record->header = header;
    record->next = NULL;
    record->pathSize = length + 1;
    memcpy(record->path, info.dli_fname, record->pathSize);
    pthread_mutex_lock(&imageLock);
    *imageTail = record;
    imageTail = &record->next;
    pthread_mutex_unlock(&imageLock);
}

// 按加载顺序移除记录，释放内存不占用本地锁。
static void LHImageRemoved(const struct mach_header *header, intptr_t slide) {
    (void)slide;
    pthread_mutex_lock(&imageLock);
    struct LHImageRecord **cursor = &imageRecords;
    while (*cursor && (*cursor)->header != header) cursor = &(*cursor)->next;
    struct LHImageRecord *removed = *cursor;
    if (removed) {
        *cursor = removed->next;
        if (!*cursor) imageTail = cursor;
    }
    pthread_mutex_unlock(&imageLock);
    free(removed);
}

// 构造阶段注册，避免首次查询的初始化锁与并发 dyld 构造函数互相等待。
__attribute__((constructor)) static void LHInitializeImageRegistry(void) {
    _dyld_register_func_for_remove_image(LHImageRemoved);
    _dyld_register_func_for_add_image(LHImageAdded);
}

// 调用者持锁，统计当次路径数组与字符串所需空间并检查溢出。
static size_t LHImagePathsSize(const void *header, size_t *count) {
    size_t size = 0;
    *count = 0;
    for (struct LHImageRecord *record = imageRecords; record; record = record->next) {
        if (header && record->header != header) continue;
        if (record->pathSize > SIZE_MAX - sizeof(char *) ||
            size > SIZE_MAX - sizeof(char *) - record->pathSize) return 0;
        size += sizeof(char *) + record->pathSize;
        (*count)++;
    }
    return size;
}

// 每次查询重新复制路径；锁外分配并重查空间，避免与 dyld/分配器形成锁循环。
char **LHCopyImagePaths(const void *header, size_t *count) {
    if (!count) return NULL;
    *count = 0;
    for (;;) {
        size_t currentCount = 0;
        pthread_mutex_lock(&imageLock);
        size_t capacity = LHImagePathsSize(header, &currentCount);
        pthread_mutex_unlock(&imageLock);
        if (!capacity) return NULL;
        char **paths = malloc(capacity);
        if (!paths) return NULL;
        pthread_mutex_lock(&imageLock);
        size_t needed = LHImagePathsSize(header, &currentCount);
        if (!needed || needed > capacity) {
            pthread_mutex_unlock(&imageLock);
            free(paths);
            if (!needed) return NULL;
            continue;
        }
        char *nextPath = (char *)(paths + currentCount);
        size_t index = 0;
        for (struct LHImageRecord *record = imageRecords; record; record = record->next) {
            if (header && record->header != header) continue;
            paths[index++] = nextPath;
            memcpy(nextPath, record->path, record->pathSize);
            nextPath += record->pathSize;
        }
        pthread_mutex_unlock(&imageLock);
        *count = currentCount;
        return paths;
    }
}

// 裸 header 只作匹配；重新打开路径并核对身份后才读取镜像内存。
struct libhooker_image *LHOpenImageForHeader(const void *header) {
    if (!header) return NULL;
    size_t count = 0;
    char **paths = LHCopyImagePaths(header, &count);
    struct libhooker_image *image = count ? LHOpenLoadedImage(paths[0]) : NULL;
    free(paths);
    if (image && image->imageHeader != header) { LHCloseImage(image); image = NULL; }
    return image;
}
