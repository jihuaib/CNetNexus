/**
 * @file   bit.h
 * @brief  通用标记位操作宏
 * @author jhb
 * @date   2026/03/07
 */
#ifndef BIT_H
#define BIT_H

#include <stdint.h>

/** 置位：将 flags 中的 bit 位设为 1 */
#define BIT_SET(flags, bit) ((flags) |= (uint32_t)(bit))

/** 清位：将 flags 中的 bit 位清为 0 */
#define BIT_CLR(flags, bit) ((flags) &= ~(uint32_t)(bit))

/** 测位：判断 flags 中的 bit 位是否为 1，返回非零值表示已设置 */
#define BIT_TEST(flags, bit) (((flags) & (uint32_t)(bit)) != 0u)

/** 翻转：将 flags 中的 bit 位取反 */
#define BIT_TOGGLE(flags, bit) ((flags) ^= (uint32_t)(bit))

#endif /* BIT_H */
