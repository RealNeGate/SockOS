#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef unsigned long long u64;
typedef uint32_t           u32;
typedef uint16_t           u16;
typedef uint8_t             u8;

typedef long long          i64;
typedef int32_t            i32;
typedef int16_t            i16;
typedef int8_t              i8;

#define ELEM_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE_ALIGN(a) (((a) + 0x1000 - 1) & -0x1000)

// common macros
//   NO_DEFAULT;
//      basically an unreachable switch statement default
#define NO_DEFAULT default: __builtin_debugtrap()
//   UNUSED(var);
#define UNUSED(a) ((void)a)

// common iterators (also macros but i logically separate them in my head)
// FOREACH_N(i, 0, 10) sum += i*i;
#define FOREACH_N(it, start, end)           for (ptrdiff_t it = (start); it != (end); it++)
// FOREACH_N_STEP(i, 0, 4) sum += i*i;
#define FOREACH_N_STEP(it, start, end, inc) for (ptrdiff_t it = (start); it != (end); it += (inc))

#ifdef __CUIK__
#define USE_INTRIN 1
#endif

// used for zig<->c embed communication... or as i like to call them
// zigma to zigma communication.
typedef struct {
    size_t length;
    const u8* data;
} Buffer;
