// 仅借用经过 Mach-O 边界检查的头部零填充，不覆盖函数或 section。
#ifndef LH_HEADER_RELAY_H
#define LH_HEADER_RELAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct LHHeaderRelay;
// 所有接口都由现有内存事务锁串行保护；这里只预留，编码仍交给影子事务写入。
struct LHHeaderRelay *LHPrepareHeaderRelay(uintptr_t target, size_t size, uintptr_t *address);
void LHCancelHeaderRelay(struct LHHeaderRelay *relay);
// 暂停线程后、提交任何页面前读取实际映射复核，不读取已修改的影子字节。
bool LHValidateHeaderRelays(void);
// 清空并填充本页待提交中继位图，调用方提供至少 ceil(pageSize / 8) 字节。
void LHGetPendingHeaderRelayMask(uintptr_t page, uint8_t *mask, size_t pageSize);
// 仍可能生效时保留预留区；未提交或完整回滚后释放，调用位于恢复线程之后。
void LHFinishHeaderRelays(bool preserve);

#ifdef __cplusplus
}
#endif
#endif
