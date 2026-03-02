#ifndef DEFINES_H
#define DEFINES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Types
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t u64;
typedef int32_t b32;
typedef float r32;
typedef double r64;
typedef int32_t s32;
typedef int64_t s64;

#define KB(n) ((u64)(n) << 10)
#define MB(n) ((u64)(n) << 20)
#define GB(n) ((u64)(n) << 30)
#define TB(n) ((u64)(n) << 40)

const r32 PI = 3.14159265359f;

#endif