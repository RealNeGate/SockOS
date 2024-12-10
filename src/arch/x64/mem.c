#include "x64.h"

uintptr_t arch_canonical_addr(uintptr_t ptr) {
    return (ptr >> 47) != 0 ? ptr | (0xFFFFull << 48) : ptr;
}

static PageTable* get_pt(PageTable* parent, size_t index) {
    if (parent->entries[index] & PAGE_PRESENT) {
        return paddr2kaddr(arch_canonical_addr(parent->entries[index] & 0xFFFFFFFFF000ull));
    } else {
        return NULL;
    }
}

static PageTable* get_or_alloc_pt(PageTable* parent, size_t index, int depth, PageFlags flags) {
    if (parent->entries[index] & PAGE_PRESENT) {
        if (flags) {
            parent->entries[index] |= flags;
        }

        return paddr2kaddr(arch_canonical_addr(parent->entries[index] & 0xFFFFFFFFF000ull));
    }

    PageTable* new_pt = kpool_alloc_page();
    kassert(((uintptr_t) new_pt & 0xFFF) == 0, "page tables must be 4KiB aligned");
    parent->entries[index] = kaddr2paddr(new_pt) | flags | PAGE_PRESENT;
    return new_pt;
}

void* memmap_view(PageTable* address_space, uintptr_t phys_addr, uintptr_t virt_addr, size_t size, VMem_Flags flags) {
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    kassert((phys_addr & 0xFFFull) == 0, "physical address unaligned (%p)", phys_addr);
    kassert((virt_addr & 0xFFFull) == 0, "virtual address unaligned (%p)", virt_addr);
    kassert(flags & 0xFFF, "invalid flags (%x)", flags);

    uint64_t page_flags = 0;
    if (flags & VMEM_PAGE_USER)  { page_flags |= PAGE_USER;  }
    if (flags & VMEM_PAGE_WRITE) { page_flags |= PAGE_WRITE; }

    // Generate the page table mapping
    bool is_current = kaddr2paddr(address_space) == x86_get_cr3();
    for (size_t i = 0; i < page_count; i++) {
        PageTable* table_l3 = get_or_alloc_pt(address_space, (virt_addr >> 39) & 0x1FF, 0, page_flags); // 512GiB
        PageTable* table_l2 = get_or_alloc_pt(table_l3,      (virt_addr >> 30) & 0x1FF, 1, page_flags); // 1GiB
        PageTable* table_l1 = get_or_alloc_pt(table_l2,      (virt_addr >> 21) & 0x1FF, 2, page_flags); // 2MiB
        size_t pte_index = (virt_addr >> 12) & 0x1FF; // 4KiB

        table_l1->entries[pte_index] = (phys_addr & 0xFFFFFFFFF000) | page_flags | PAGE_PRESENT;
        if (is_current) {
            asm volatile("invlpg [%0]" ::"r" (virt_addr) : "memory");
        }

        virt_addr += PAGE_SIZE, phys_addr += PAGE_SIZE;
    }

    return (void*) virt_addr;
}

void memmap__unview(PageTable* address_space, uintptr_t virt_addr, size_t size) {
    size_t page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    kassert((virt_addr & 0xFFFull) == 0, "virtual address unaligned (%p)", virt_addr);

    // Generate the page table mapping
    bool is_current = kaddr2paddr(address_space) == x86_get_cr3();
    for (size_t i = 0; i < page_count; i++, virt_addr += PAGE_SIZE) {
        PageTable* table_l3 = get_pt(address_space, (virt_addr >> 39) & 0x1FF); // 512GiB
        if (table_l3 == NULL) { continue; }
        PageTable* table_l2 = get_pt(table_l3,      (virt_addr >> 30) & 0x1FF); // 1GiB
        if (table_l2 == NULL) { continue; }
        PageTable* table_l1 = get_pt(table_l2,      (virt_addr >> 21) & 0x1FF); // 2MiB
        if (table_l1 == NULL) { continue; }

        size_t pte_index = (virt_addr >> 12) & 0x1FF; // 4KiB
        table_l1->entries[pte_index] = 0;
        if (is_current) {
            asm volatile("invlpg [%0]" ::"r" (virt_addr) : "memory");
        }
    }
}

static void dump_pages(PageTable* pt, int depth, uintptr_t base) {
    static const uint64_t shifts[4] = { 39, 30, 21, 12 };
    for (int i = 0; i < 512; i++) {
        if (pt->entries[i] & 1) {
            for (int i = 0; i < depth; i++) { kprintf("  "); }

            uintptr_t vaddr = base + (i << shifts[depth]);
            kprintf("[%d] %p (%p)\n", i, pt->entries[i], vaddr);

            if (depth < 1) {
                dump_pages(paddr2kaddr(pt->entries[i] & -PAGE_SIZE), depth + 1, vaddr);
            }
        }
    }
}

static void memdump(u64 *buffer, size_t size) {
    int scale = 16;
    int max_pixel = boot_info->fb.width * boot_info->fb.height;
    int width = boot_info->fb.width;

    for (int i = 0; i < size; i++) {
        u32 color = 0xFF000000 | ((buffer[i] > 0) ? buffer[i] : 0xFF050505);

        for (int y = 0; y < scale; y++) {
            for (int x = 0; x < scale; x++) {

                int sx = ((i * scale) % width) + x;
                int sy = (((i * scale) / width) * scale) + y;
                int idx = (sy * width) + sx;
                if (idx >= max_pixel) return;

                boot_info->fb.pixels[idx] = color;
            }
        }
    }
}

static u64 memmap__probe(PageTable* address_space, uintptr_t virt) {
    size_t l[4] = {
        (virt >> 39) & 0x1FF,
        (virt >> 30) & 0x1FF,
        (virt >> 21) & 0x1FF,
        (virt >> 12) & 0x1FF,
    };

    PageTable* curr = address_space;
    for (size_t i = 0; i < 3; i++) {
        // kprintf("Travel %x %x %x\n", (int)(uintptr_t) curr, (int) i, curr->entries[l[i]]);
        if (curr->entries[l[i]] == 0) {
            // kprintf("Didn't find page!!!\n");
            return 0;
        }

        curr = paddr2kaddr(arch_canonical_addr(curr->entries[l[i]] & 0xFFFFFFFFF000ull));
    }

    return curr->entries[l[3]];
}

bool memmap_translate(PageTable* address_space, uintptr_t virt, u64* out) {
    u64 r = memmap__probe(address_space, virt);
    if ((r & PAGE_PRESENT) == 0) {
        return false;
    }

    *out = (r & ~0xFFFull) | (virt & 0xFFF);
    return true;
}

