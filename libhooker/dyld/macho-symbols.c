// 有界 Mach-O 解析与符号表查询；不负责镜像加载或磁盘缓存映射。
#include "macho-symbols.h"
#include <mach-o/loader.h>
#include <ptrauth.h>
#include <string.h>

// 遍历经过边界检查的 Mach-O 加载命令。
static const struct load_command *LHCommand(const struct mach_header_64 *header, const uint8_t *cursor) {
    const uint8_t *begin = (const uint8_t *)(header + 1);
    if (cursor < begin || ((uintptr_t)cursor & 7) ||
        !LHRange(cursor - begin, 1, sizeof(struct load_command), header->sizeofcmds)) return NULL;
    const struct load_command *command = (const void *)cursor;
    if (command->cmdsize < sizeof(*command) || (command->cmdsize & 7) ||
        !LHRange(cursor - begin, command->cmdsize, 1, header->sizeofcmds)) return NULL;
    return command;
}

// 从 __TEXT 计算 slide，不依赖 dyld 的 C++ 对象布局。
uintptr_t LHImageSlide(const void *image) {
    const struct mach_header_64 *header = image;
    if (!header || header->magic != MH_MAGIC_64) return UINTPTR_MAX;
    const uint8_t *cursor = (const void *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *command = LHCommand(header, cursor);
        if (!command) return UINTPTR_MAX;
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const void *)command;
            if (!strncmp(segment->segname, "__TEXT", 16)) return (uintptr_t)header - segment->vmaddr;
        }
        cursor += command->cmdsize;
    }
    return UINTPTR_MAX;
}

// 仅为指令段中的符号添加 PAC，保留数据符号的原始地址。
static void *LHSignSymbol(const void *image, uint64_t value, uintptr_t slide) {
    const struct mach_header_64 *header = image;
    const uint8_t *cursor = (const void *)(header + 1);
    void *pointer = (void *)(value + slide);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *command = LHCommand(header, cursor);
        if (!command) break;
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *segment = (const void *)command;
            if (LHRange(sizeof(*segment), segment->nsects, sizeof(struct section_64), command->cmdsize)) {
                const struct section_64 *sections = (const void *)(segment + 1);
                for (uint32_t j = 0; j < segment->nsects; j++) {
                    if (value >= sections[j].addr && value - sections[j].addr < sections[j].size &&
                        (sections[j].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS))) {
                        return ptrauth_sign_unauthenticated(pointer, ptrauth_key_function_pointer, 0);
                    }
                }
            }
        }
        cursor += command->cmdsize;
    }
    return pointer;
}

// 在有长度限制的 nlist 与字符串表中查找符号。
void LHSearchTable(const void *image, uintptr_t slide, const struct nlist_64 *symbols,
                          size_t count, const char *strings, size_t stringSize,
                          const char **names, void **results, size_t wanted) {
    size_t remaining = 0;
    for (size_t i = 0; i < wanted; i++) remaining += !results[i] && names[i];
    if (!remaining) return;
    for (size_t i = 0; i < count; i++) {
        // 磁盘符号区可能未对齐，复制固定项后再访问，避免未对齐结构体读取。
        struct nlist_64 symbol;
        memcpy(&symbol, (const uint8_t *)symbols + i * sizeof(symbol), sizeof(symbol));
        if ((symbol.n_type & N_STAB) || (symbol.n_type & N_TYPE) != N_SECT || !symbol.n_value) continue;
        uint32_t offset = symbol.n_un.n_strx;
        if (!offset || offset >= stringSize || !memchr(strings + offset, 0, stringSize - offset)) continue;
        for (size_t j = 0; j < wanted; j++) {
            if (!results[j] && names[j] && !strcmp(strings + offset, names[j])) {
                results[j] = LHSignSymbol(image, symbol.n_value, slide);
                if (!--remaining) return;
            }
        }
    }
}

// 在内存 Mach-O 中查找普通符号，补充导出表和本地缓存查询。
void LHSearchImage(const void *image, uintptr_t slide, const char **names, void **results, size_t count) {
    if (LHAllSymbolsFound(results, count)) return;
    const struct mach_header_64 *header = image;
    if (!header || header->magic != MH_MAGIC_64) return;
    const struct symtab_command *symtab = NULL;
    const struct segment_command_64 *linkedit = NULL;
    const uint8_t *cursor = (const void *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        const struct load_command *command = LHCommand(header, cursor);
        if (!command) return;
        if (command->cmd == LC_SYMTAB && command->cmdsize >= sizeof(*symtab)) symtab = (const void *)command;
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(*linkedit)) {
            const struct segment_command_64 *segment = (const void *)command;
            if (!strncmp(segment->segname, "__LINKEDIT", 16)) linkedit = segment;
        }
        cursor += command->cmdsize;
    }
    if (!symtab || !linkedit || symtab->symoff < linkedit->fileoff || symtab->stroff < linkedit->fileoff ||
        !LHRange(symtab->symoff - linkedit->fileoff, symtab->nsyms, sizeof(struct nlist_64), linkedit->filesize) ||
        !LHRange(symtab->stroff - linkedit->fileoff, symtab->strsize, 1, linkedit->filesize)) return;
    uintptr_t base = slide + linkedit->vmaddr - linkedit->fileoff;
    LHSearchTable(image, slide, (const void *)(base + symtab->symoff), symtab->nsyms,
                  (const char *)(base + symtab->stroff), symtab->strsize, names, results, count);
}
