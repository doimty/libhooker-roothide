// 用隔离 dyld 桩运行真实镜像打开逻辑，不加载插件或依赖宿主共享缓存。
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define REQUIRE(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "image-lookup failure at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

static const char iosurfacePath[] = "/System/Library/Frameworks/IOSurface.framework/IOSurface";
static const char dependencyPath[] = "/fixtures/dependencies/IOSurface";
static const char aliasPath[] = "/fixtures/aliases/IOSurface";
static const char diskPath[] = "/fixtures/disk/IOSurface";
static const char unloadedPath[] = "/fixtures/unloaded/IOSurface";
static char handleToken;

struct TestMachO {
    struct mach_header_64 header;
    struct segment_command_64 text;
};
static struct TestMachO dependency, target;
static struct {
    struct { const char *path; const void *header; } images[3];
    uint32_t imageCount;
    const char *openPath;
    const void *spiHeader;
    bool openSucceeds, spiAvailable, diskAlias;
    bool forkHooksAvailable, stillForking;
    unsigned forkLookups, forkPrepares, forkParents, rejectedDuringFork;
    unsigned opens, closes, references, spiLookups, spiCalls;
    unsigned counts, names, headers, realpaths;
} mock;

// 返回不透明句柄，并要求调用者始终采用只查询已加载镜像的 flags。
static void *mockDlopen(const char *path, int flags) {
    REQUIRE(path && !strcmp(path, mock.openPath));
    REQUIRE(flags == (RTLD_LAZY | RTLD_LOCAL | RTLD_FIRST | RTLD_NOLOAD));
    mock.opens++;
    if (mock.stillForking) { mock.rejectedDuringFork++; return NULL; }
    if (!mock.openSucceeds) return NULL;
    REQUIRE(mock.references == 0);
    mock.references++;
    return &handleToken;
}

static int mockDlclose(void *handle) {
    REQUIRE(handle == &handleToken && mock.references == 1);
    mock.references--;
    mock.closes++;
    return 0;
}

static const void *mockDlopenHeader(void *handle) {
    REQUIRE(handle == &handleToken && mock.references == 1);
    mock.spiCalls++;
    return mock.spiHeader;
}

static void mockForkPrepare(void) {
    mock.forkPrepares++;
    mock.stillForking = true;
}

static void mockForkParent(void) {
    mock.forkParents++;
    mock.stillForking = false;
}

// 仅替换被测源码的解析器；不覆盖宿主 libSystem 的 dlsym。
static void *mockDlsym(void *handle, const char *name) {
    REQUIRE(handle == RTLD_DEFAULT);
    if (!strcmp(name, "_dyld_get_dlopen_image_header")) {
        REQUIRE(mock.references == 1);
        mock.spiLookups++;
        return mock.spiAvailable ? (void *)mockDlopenHeader : NULL;
    }
    if (!strcmp(name, "_dyld_dlopen_atfork_prepare")) {
        mock.forkLookups++;
        return mock.forkHooksAvailable ? (void *)mockForkPrepare : NULL;
    }
    REQUIRE(!strcmp(name, "_dyld_dlopen_atfork_parent"));
    mock.forkLookups++;
    return mock.forkHooksAvailable ? (void *)mockForkParent : NULL;
}

static uint32_t mockImageCount(void) {
    REQUIRE(mock.references == 1);
    mock.counts++;
    return mock.imageCount;
}

static const char *mockImageName(uint32_t index) {
    REQUIRE(mock.references == 1 && index < mock.imageCount);
    mock.names++;
    return mock.images[index].path;
}

static const struct mach_header *mockImageHeader(uint32_t index) {
    REQUIRE(mock.references == 1 && index < mock.imageCount);
    mock.headers++;
    return mock.images[index].header;
}

// 缓存路径没有独立文件；另一个场景允许磁盘别名解析到同一规范路径。
static char *mockRealpath(const char *path, char *resolved) {
    REQUIRE(path && resolved && mock.references == 1);
    mock.realpaths++;
    if (mock.diskAlias && (!strcmp(path, aliasPath) || !strcmp(path, diskPath))) {
        REQUIRE(sizeof(diskPath) <= PATH_MAX);
        memcpy(resolved, diskPath, sizeof(diskPath));
        return resolved;
    }
    errno = ENOENT;
    return NULL;
}

#define dlopen mockDlopen
#define dlclose mockDlclose
#define dlsym mockDlsym
#define _dyld_image_count mockImageCount
#define _dyld_get_image_name mockImageName
#define _dyld_get_image_header mockImageHeader
#define realpath mockRealpath
#ifndef LH_DYLD_TEST_SOURCE
#define LH_DYLD_TEST_SOURCE "../libhooker/dyld/dyldSyms-modern.c"
#endif
#include LH_DYLD_TEST_SOURCE
#undef realpath
#undef _dyld_get_image_header
#undef _dyld_get_image_name
#undef _dyld_image_count
#undef dlsym
#undef dlclose
#undef dlopen

// 镜像头和 __TEXT 命令真实交给 LHImageSlide 解析，使用可预测的非零 slide。
static void initializeMachO(struct TestMachO *image, uintptr_t slide) {
    *image = (struct TestMachO){
        .header = {.magic = MH_MAGIC_64, .filetype = MH_DYLIB,
            .ncmds = 1, .sizeofcmds = sizeof(image->text)},
        .text = {.cmd = LC_SEGMENT_64, .cmdsize = sizeof(image->text),
            .segname = "__TEXT", .vmaddr = (uintptr_t)&image->header - slide,
            .vmsize = 0x1000},
    };
}

static void resetFixture(void) {
    REQUIRE(mock.references == 0);
    memset(&mock, 0, sizeof(mock));
    initializeMachO(&dependency, 0x1000);
    initializeMachO(&target, 0x4000);
    target.header.flags = MH_DYLIB_IN_CACHE;
    mock.imageCount = 3;
    // 同 basename 的普通依赖排在目标前面，中间空名字也必须跳过。
    mock.images[0].path = dependencyPath;
    mock.images[0].header = &dependency.header;
    mock.images[2].path = iosurfacePath;
    mock.images[2].header = &target.header;
    mock.openPath = iosurfacePath;
    mock.openSucceeds = true;
}

static void requireTargetAndClose(struct libhooker_image *image) {
    REQUIRE(image && image->imageHeader == &target.header);
    REQUIRE(image->slide == 0x4000 && image->dyldHandle == &handleToken);
    REQUIRE(mock.opens == 1 && mock.references == 1 && mock.closes == 0);
    LHCloseImage(image);
    REQUIRE(mock.references == 0 && mock.closes == 1);
}

// 复现 iOS 15：缺少 header SPI，NOLOAD 成功，IOSurface 的 realpath 必定 ENOENT。
static void testCacheOnlyImage(void) {
    resetFixture();
    struct libhooker_image *image = LHOpenLoadedImage(iosurfacePath);
    REQUIRE(mock.spiLookups == 1 && mock.spiCalls == 0);
    REQUIRE(mock.realpaths == 0);
    requireTargetAndClose(image);
}

// 后期 dyld 的 fork SPI 会改变状态，不得把它们当成通用查询锁。
static void testPresentForkHooks(void) {
    resetFixture();
    mock.forkHooksAvailable = true;
    struct libhooker_image *image = LHOpenLoadedImage(iosurfacePath);
    requireTargetAndClose(image);
    REQUIRE(mock.forkLookups == 0 && mock.forkPrepares == 0 && mock.forkParents == 0);
    REQUIRE(!mock.stillForking && mock.rejectedDuringFork == 0);

    resetFixture();
    mock.forkHooksAvailable = true;
    mock.openSucceeds = false;
    REQUIRE(LHOpenLoadedImage(iosurfacePath) == NULL);
    REQUIRE(mock.forkLookups == 0 && mock.forkPrepares == 0 && mock.forkParents == 0);
    REQUIRE(!mock.stillForking && mock.rejectedDuringFork == 0 && !mock.references);
}

// 完整路径匹配不能破坏原有的磁盘符号链接/别名回退。
static void testDiskAlias(void) {
    resetFixture();
    mock.openPath = aliasPath;
    mock.images[2].path = diskPath;
    mock.diskAlias = true;
    struct libhooker_image *image = LHOpenLoadedImage(aliasPath);
    REQUIRE(mock.realpaths >= 2);
    requireTargetAndClose(image);
}

// 新系统由 SPI 直接给出镜像头，完全不需要枚举或磁盘解析。
static void testHeaderSPI(void) {
    resetFixture();
    mock.spiAvailable = true;
    mock.spiHeader = &target.header;
    mock.imageCount = 0;
    struct libhooker_image *image = LHOpenLoadedImage(iosurfacePath);
    REQUIRE(mock.spiCalls == 1 && !mock.counts && !mock.names && !mock.headers && !mock.realpaths);
    requireTargetAndClose(image);

    // SPI 存在但未给出头时，仍按已加载的完整路径回退。
    resetFixture();
    mock.spiAvailable = true;
    image = LHOpenLoadedImage(iosurfacePath);
    REQUIRE(mock.spiCalls == 1 && mock.realpaths == 0);
    requireTargetAndClose(image);
}

// 未加载请求不能改成普通 dlopen；失败句柄不能进入镜像枚举。
static void testUnloadedImage(void) {
    resetFixture();
    mock.openPath = unloadedPath;
    mock.openSucceeds = false;
    REQUIRE(LHOpenLoadedImage(unloadedPath) == NULL);
    REQUIRE(mock.opens == 1 && !mock.references && !mock.closes);
    REQUIRE(!mock.spiLookups && !mock.counts && !mock.realpaths);

    resetFixture();
    REQUIRE(LHOpenLoadedImage(NULL) == NULL);
    LHCloseImage(NULL);
    REQUIRE(!mock.opens && !mock.closes);
}

// 即使取得句柄，找不到相应镜像头也必须归还引用，不能误用同 basename 的依赖。
static void testMissingHeader(void) {
    resetFixture();
    mock.imageCount = 2;
    REQUIRE(LHOpenLoadedImage(iosurfacePath) == NULL);
    REQUIRE(mock.opens == 1 && mock.closes == 1 && !mock.references);
    REQUIRE(mock.realpaths == 1 && !mock.headers);
}

// 真实 Mach-O 校验拒绝错误 magic、缺少 __TEXT 和坏命令，所有失败均释放引用。
static void testInvalidMachO(void) {
    for (unsigned failure = 0; failure < 4; failure++) {
        resetFixture();
        if (failure == 0) target.header.magic = MH_MAGIC;
        if (failure == 1) memcpy(target.text.segname, "__DATA", sizeof("__DATA"));
        if (failure == 2) target.text.cmdsize = 0;
        if (failure == 3) {
            target.header.magic = 0;
            mock.spiAvailable = true;
            mock.spiHeader = &target.header;
            mock.imageCount = 0;
        }
        REQUIRE(LHOpenLoadedImage(iosurfacePath) == NULL);
        REQUIRE(mock.opens == 1 && mock.closes == 1 && !mock.references);
    }
}

int main(void) {
    testPresentForkHooks();
    testCacheOnlyImage();
    testDiskAlias();
    testHeaderSPI();
    testUnloadedImage();
    testMissingHeader();
    testInvalidMachO();
    printf("image-lookup: %u checks passed\n", checks);
    return 0;
}
