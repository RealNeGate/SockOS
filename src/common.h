#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

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

#define CONCAT_(x, y) x ## y
#define CONCAT(x, y) CONCAT_(x, y)

#define STR2(x) #x
#define STR(x) STR2(x)

// common macros
//   NO_DEFAULT;
//      basically an unreachable switch statement default
#define NO_DEFAULT default: __builtin_debugtrap()
//   UNUSED(var);
#define UNUSED(a) ((void)a)

// common iterators (also macros but i logically separate them in my head)
// FOREACH_N(i, 0, 10) sum += i*i;
#define FOR_N(it, start, end)           for (ptrdiff_t it = (start); it != (end); it++)

// logging options
#define DEBUG_SYSCALL 1
#define DEBUG_IRQ     1
#define DEBUG_PCI     0
#define DEBUG_ENV     0
#define DEBUG_VMEM    0
#define DEBUG_KHEAP   0
#define DEBUG_KPOOL   0
#define DEBUG_SCHED   0
#define DEBUG_NBHM    0
#define DEBUG_SPALL   1

#define ON_DEBUG(cond) CONCAT(DO_IF_, CONCAT(DEBUG_, cond))

#define DO_IF(cond) CONCAT(DO_IF_, cond)
#define DO_IF_0(...)
#define DO_IF_1(...) __VA_ARGS__

#ifdef __CUIK__
#define USE_INTRIN 1
#endif

// bootleg string.h
void* memset(void* buffer, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
bool memeq(const void* a, const void* b, size_t n);

////////////////////////////////
// Spin-lock
////////////////////////////////
typedef _Atomic(uint32_t) Lock;
void spin_lock(Lock* lock);
void spin_unlock(Lock* lock);

// bootleg stdio.h
void kprintf(const char *fmt, ...);
void print_ring_init(void);

uint64_t get_time_ticks(void);

#define kassert(cond, ...) ((cond) ? 0 : (kprintf("%s:%d: assertion failed!\n  %s\n  ", __FILE__, __LINE__, #cond), kprintf(__VA_ARGS__), kprintf("\n\n"), __builtin_trap()))
#define panic(...) (kprintf("%s:%d: panic!\n", __FILE__, __LINE__), kprintf(__VA_ARGS__), __builtin_trap())
