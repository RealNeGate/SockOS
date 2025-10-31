////////////////////////////////
// EBR - Epoch-based reclamation
////////////////////////////////
#ifndef EBR_H
#define EBR_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#define EBR_VIRTUAL_ALLOC(size)     memset(kheap_alloc(size), 0, size)
#define EBR_VIRTUAL_FREE(ptr, size) kheap_free(ptr, size)

void ebr_init(void);
void ebr_deinit(void);

// Annotates the critical sections in the mutators
void ebr_enter_cs(void);
void ebr_exit_cs(void);

void ebr_free(void* ptr, size_t size);

#define EBR_DEBOOGING 1

#if EBR_DEBOOGING
#define EBR__BEGIN(name)      spall_begin_event(name, cpu_get_index())
#define EBR__END()            spall_end_event(cpu_get_index())
#else
#define EBR__BEGIN(name)
#define EBR__END()
#endif

#endif // EBR_H

#ifdef EBR_IMPL
#undef EBR_IMPL

// for the time in the ebr entry
#define EBR_PINNED_BIT (1ull << 63ull)

#ifdef _WIN32
#include <tlhelp32.h>
#endif

static struct {
    _Alignas(64) _Atomic(uint64_t) time;
    _Alignas(64) _Atomic(uint64_t) checkpoint_t;
} cores[MAX_CORES];

typedef struct EBR_FreeNode EBR_FreeNode;
struct EBR_FreeNode {
    EBR_FreeNode* next;
    // space to reclaim
    void* ptr;
    size_t size;
};

// concurrent free-list
static _Atomic(EBR_FreeNode*) ebr_free_list;

void ebr_deinit(void) {
}

extern void x86_sti(void);
static int ebr_thread_fn(void* arg) {
    EBR_FreeNode* last_free_list = NULL;
    for (;;) {
        #if EBR_DEBOOGING
        spall_begin_event("Cycle", -1);
        #endif

        // clear the free list, then we wait for the checkpoint before
        // freeing the memory.
        EBR_FreeNode* free_list = atomic_exchange(&ebr_free_list, NULL);
        if (free_list != NULL) {
            #if EBR_DEBOOGING
            spall_begin_event("Checkpoint", -1);
            #endif

            // wait for the critical sections to advance, once
            // that happens we can choose to free things.
            for (int i = 0; i < boot_info->core_count; i++) {
                uint64_t before_t = atomic_load_explicit(&boot_info->cores[i].ebr_checkpoint, memory_order_acquire);
                if (before_t & EBR_PINNED_BIT) {
                    uint64_t now_t, tries = 0;
                    do {
                        // once we're at this a bunch of times, we start to yield
                        if (tries > 4) {
                            // thrd_yield();
                        }

                        // idk, maybe this should be a better spinlock
                        now_t = atomic_load_explicit(&boot_info->cores[i].ebr_time, memory_order_acquire);
                        tries++;
                    } while (before_t == now_t);

                    atomic_store_explicit(&boot_info->cores[i].ebr_checkpoint, now_t, memory_order_release);
                }
            }

            #if EBR_DEBOOGING
            spall_end_event(-1);
            spall_begin_event("Reclaim memory", -1);
            #endif

            if (last_free_list) {
                EBR_FreeNode* list = last_free_list;
                while (list) {
                    EBR_FreeNode* next = list->next;
                    kheap_free(list, sizeof(EBR_FreeNode));
                    list = next;
                }
                last_free_list = NULL;
            }

            // empty the free list, it's possible that the mutators are still watching it
            // so we can't free it until the next iteration.
            for (; free_list; free_list = free_list->next) {
                EBR_VIRTUAL_FREE(free_list->ptr, free_list->size);
            }
            last_free_list = free_list;

            #if EBR_DEBOOGING
            spall_end_event(-1);
            #endif
        }

        #if EBR_DEBOOGING
        spall_end_event(-1);
        #endif

        // Sleep for a while
        sleep(1000000);
    }

    return 0;
}

void ebr_init(void) {
    Thread* t = thread_create(NULL, ebr_thread_fn, 0, (uintptr_t) kheap_alloc(KERNEL_STACK_SIZE), KERNEL_STACK_SIZE);
    thread_resume(t);
}

void ebr_free(void* ptr, size_t size) {
    EBR_FreeNode* node = kheap_alloc(sizeof(EBR_FreeNode));
    node->ptr  = ptr;
    node->size = size;

    EBR_FreeNode* list = ebr_free_list;
    do {
        node->next = list;
    } while (!atomic_compare_exchange_strong(&ebr_free_list, &list, node));
}

void ebr_enter_cs(void) {
    // flips the top bit on
    PerCPU* cpu = cpu_get();
    uint64_t t = atomic_load_explicit(&cpu->ebr_time, memory_order_relaxed);
    atomic_store_explicit(&cpu->ebr_time, t + EBR_PINNED_BIT, memory_order_release);
}

// flips the top bit off AND increments time by one
void ebr_exit_cs(void) {
    PerCPU* cpu = cpu_get();
    uint64_t t = atomic_load_explicit(&cpu->ebr_time, memory_order_relaxed);
    atomic_store_explicit(&cpu->ebr_time, t + EBR_PINNED_BIT + 1, memory_order_release);
}

#endif // EBR_IMPL
