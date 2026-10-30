// 用真实符号解析与单个 Mach 接口桩验证共享缓存查询的入口筛选。
#include <assert.h>
#include <mach/mach.h>
#include <stddef.h>
#include <stdio.h>

static unsigned taskInfoCalls;

// 阻止访问宿主缓存，使初始化次数和后续测试符号表完全可控。
static kern_return_t LHTestTaskInfo(task_name_t task, task_flavor_t flavor,
        task_info_t info, mach_msg_type_number_t *count) {
    (void)task; (void)flavor; (void)info; (void)count;
    taskInfoCalls++;
    return KERN_FAILURE;
}

#define task_info LHTestTaskInfo
#include "../libhooker/dyld/dyldSyms-modern.c"
#undef task_info

// 普通镜像不初始化缓存；共享缓存仍初始化一次并按原有表格式解析符号。
int main(void) {
    struct {
        struct dyld_cache_header cache;
        struct mach_header_64 image;
    } fixture = {.image = {.magic = MH_MAGIC_64}};
    const char *name = "_cached";
    void *result = NULL;
    LHSearchCache(&fixture.image, 0, &name, &result, 1);
    assert(!result && taskInfoCalls == 0 && !cacheHeader && !localSymbols);

    fixture.image.flags = MH_DYLIB_IN_CACHE;
    LHSearchCache(&fixture.image, 0, &name, &result, 1);
    assert(!result && taskInfoCalls == 1);

    struct {
        struct dyld_cache_local_symbols_entry_64 entry;
        struct nlist_64 symbol;
        char strings[9];
    } table = {
        .entry = {.dylibOffset = offsetof(typeof(fixture), image), .nlistCount = 1},
        .symbol = {.n_un.n_strx = 1, .n_type = N_SECT, .n_value = 0x1000},
        .strings = "\0_cached",
    };
    cacheHeader = &fixture.cache;
    localSymbols = (const void *)&table;
    localInfo = (struct dyld_cache_local_symbols_info){
        .nlistOffset = offsetof(typeof(table), symbol), .nlistCount = 1,
        .stringsOffset = offsetof(typeof(table), strings), .stringsSize = sizeof(table.strings),
        .entriesOffset = offsetof(typeof(table), entry), .entriesCount = 1,
    };
    wideEntries = true;
    LHSearchCache(&fixture.image, 0x2000, &name, &result, 1);
    assert(result == (void *)0x3000 && taskInfoCalls == 1);

    fixture.image.flags = 0;
    result = NULL;
    LHSearchCache(&fixture.image, 0x2000, &name, &result, 1);
    assert(!result && taskInfoCalls == 1);
    puts("symbol-cache: cache filtering and lookup passed");
    return 0;
}
