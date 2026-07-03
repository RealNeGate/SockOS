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
#define FOR_N(it, start, end) for (ptrdiff_t it = (start); it != (end); it++)
#define FOR_REV_N(it, start, end) for (ptrdiff_t it = (end); (it--) != (start);)

// logging options
#define DEBUG_SYSCALL 0
#define DEBUG_IRQ     0
#define DEBUG_PCI     0
#define DEBUG_ENV     0
#define DEBUG_VMEM    0
#define DEBUG_KHEAP   0
#define DEBUG_SCHED   0
#define DEBUG_NBHM    0
#define DEBUG_SPALL   0
#define DEBUG_EFI     0

#define ON_DEBUG(cond) CONCAT(DO_IF_, CONCAT(DEBUG_, cond))

#define DO_IF(cond) CONCAT(DO_IF_, cond)
#define DO_IF_0(...)
#define DO_IF_1(...) __VA_ARGS__

#ifdef __CUIK__
#define USE_INTRIN 1
#endif

typedef _Atomic(u32) atomic_u32;
typedef _Atomic(u64) atomic_u64;

// bootleg string.h
void* memset(void* buffer, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
bool memeq(const void* a, const void* b, size_t n);

#define atomic_ldacq(ptr) atomic_load_explicit(ptr, memory_order_acquire)
#define atomic_strel(ptr) atomic_store_explicit(ptr, val, memory_order_release)
#define atomic_add_acq_rel(addr, src) atomic_fetch_add_explicit(addr, src, memory_order_acq_rel)

#define atomic_cas_acq_rel(addr, old, new) atomic_compare_exchange_strong_explicit(addr, old, new, memory_order_acq_rel, memory_order_acquire)

