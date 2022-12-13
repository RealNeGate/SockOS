#pragma once
#include <stdint.h>
#include <stdbool.h>

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
