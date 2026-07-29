#pragma once
#include <stdint.h>
#if defined(__linux__)
#include <endian.h>
#include <byteswap.h>
#define _BYTE_ORDER __BYTE_ORDER
#define _LITTLE_ENDIAN __LITTLE_ENDIAN
#define _BIG_ENDIAN __BIG_ENDIAN
#define bswap16(x) __bswap_16(x)
#define bswap32(x) __bswap_32(x)
#define bswap64(x) __bswap_64(x)
#elif defined(__APPLE__)
#include <machine/endian.h>
#include <libkern/OSByteOrder.h>
#define _BYTE_ORDER BYTE_ORDER
#define _LITTLE_ENDIAN LITTLE_ENDIAN
#define _BIG_ENDIAN BIG_ENDIAN
#define bswap16(x) OSSwapInt16(x)
#define bswap32(x) OSSwapInt32(x)
#define bswap64(x) OSSwapInt64(x)
#else
#define _BYTE_ORDER 1234
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t bswap64(uint64_t x) { return __builtin_bswap64(x); }
#endif
