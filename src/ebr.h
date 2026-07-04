////////////////////////////////
// EBR - Epoch-based reclamation
////////////////////////////////
#ifndef EBR_H
#define EBR_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

// Used by both data structures for tracking frozen values
#define EBR_PRIME_BIT (1ull << 63ull)

#define EBR_VIRTUAL_ALLOC(size)     memset(kheap_alloc(size), 0, size)
#define EBR_VIRTUAL_FREE(ptr, size) kheap_free(ptr, size)

// traditional heap ops
#ifndef EBR_REALLOC
#define EBR_REALLOC(ptr, size) realloc(ptr, size)
#endif // EBR_REALLOC

#define EBR_DEBOOGING 0

#if EBR_DEBOOGING
#define EBR__BEGIN(name)      spall_begin_event(name, -1)
#define EBR__END()            spall_end_event()
#else
#define EBR__BEGIN(name)
#define EBR__END()
#endif

void ebr_init(void);

// Annotates the critical sections in the mutators
void ebr_enter_cs(void);
void ebr_exit_cs(void);

void ebr_free(void* ptr, size_t size);

#endif // EBR_H

#ifdef EBR_IMPL
#undef EBR_IMPL

// for the time in the ebr entry
#define EBR_PINNED_BIT (1ull << 63ull)

typedef struct EBR_FreeNode EBR_FreeNode;
struct EBR_FreeNode {
    EBR_FreeNode* next;
    // space to reclaim
    void* ptr;
    size_t size;
};

// concurrent free-list
static _Atomic(EBR_FreeNode*) ebr_free_list;

static int ebr_thread_fn(void* arg) {
    EBR_FreeNode* last_free_list = NULL;

    #if EBR_DEBOOGING
    spall_auto_thread_init(37373737, SPALL_DEFAULT_BUFFER_SIZE);
    #endif

    for (;;) {
        // clear the free list, then we wait for the checkpoint before
        // freeing the memory.
        EBR_FreeNode* free_list = atomic_exchange(&ebr_free_list, NULL);
        if (free_list != NULL) {
            EBR__BEGIN("Checkpoint");
            // wait for the critical sections to advance, once
            // that happens we can choose to free things.
            for (int i = 0; i < boot_info->core_count; i++) {
                uint64_t time = atomic_ldacq(&boot_info->cores[i].ebr_time);
                atomic_strel(&boot_info->cores[i].ebr_checkpoint, time);
            }

            for (int i = 0; i < boot_info->core_count; i++) {
                uint64_t before_t = atomic_ldacq(&boot_info->cores[i].ebr_checkpoint);
                if (before_t & EBR_PINNED_BIT) {
                    uint64_t now_t, tries = 0;
                    do {
                        // once we're at this a bunch of times, we start to yield
                        if (tries > 30) {
                            thread_sleep(20000);
                        }

                        // idk, maybe this should be a better spinlock
                        now_t = atomic_ldacq(&boot_info->cores[i].ebr_time);
                        tries++;
                    } while (before_t == now_t);

                    atomic_strel(&boot_info->cores[i].ebr_checkpoint, now_t);
                }
            }
            EBR__END();

            EBR__BEGIN("Reclaim memory");
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
                // printf("FREE %p %zu\n", free_list->ptr, free_list->size);
            }
            last_free_list = free_list;
            EBR__END();
        }

        // Sleep for a while
        thread_sleep(500000);
    }
}

static _Atomic bool init;
void ebr_init(void) {
    if (atomic_cas_acq_rel(&init, &(bool){ false }, true)) {
        Thread* t = thread_create(NULL, ebr_thread_fn, 0, (uintptr_t) kheap_alloc(KERNEL_STACK_SIZE), KERNEL_STACK_SIZE);
        thread_resume(t, NULL);
    }
}

void ebr_free(void* ptr, size_t size) {
    EBR_FreeNode* node = kheap_alloc(sizeof(EBR_FreeNode));
    node->ptr  = ptr;
    node->size = size;

    EBR_FreeNode* list = ebr_free_list;
    do {
        node->next = list;
    } while (!atomic_compare_exchange_strong(&ebr_free_list, &list, node));

    // printf("FREE @ %p %#zx %p\n", ebr_thread_entry, ebr_thread_entry->time, ptr);
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
