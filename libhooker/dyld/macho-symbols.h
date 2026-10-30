#ifndef LH_MACHO_SYMBOLS_H
#define LH_MACHO_SYMBOLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mach-o/nlist.h>

// 用除法检查文件范围，避免乘法和偏移相加溢出。
static inline bool LHRange(size_t offset, size_t count, size_t width, size_t size) {
    return width && offset <= size && count <= (size - offset) / width;
}

// 判断当前查询是否已全部完成，避免重复遍历镜像及磁盘符号表。
static inline bool LHAllSymbolsFound(void *const *results, size_t count) {
    for (size_t i = 0; i < count; i++) if (!results[i]) return false;
    return true;
}

uintptr_t LHImageSlide(const void *image);
void LHSearchTable(const void *image, uintptr_t slide, const struct nlist_64 *symbols,
                   size_t count, const char *strings, size_t stringSize,
                   const char **names, void **results, size_t wanted);
void LHSearchImage(const void *image, uintptr_t slide, const char **names, void **results, size_t count);
#endif
