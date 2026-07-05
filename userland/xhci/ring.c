
enum { PAGE_SIZE_DWORDS = 4096 / 4 };
static void ring_alloc(HCI_Ring* ring, size_t cnt) {
    size_t size = cnt*16;
    // round to page
    size = (size + 4095) & ~4095ull;

    ring->cycle_bit = true;
    ring->count = cnt;

    ring->base = mmap(0, 0, 0, size, PROT_READ | PROT_WRITE, 0);
    ring->base_paddr = syscall(SYS_get_paddr, ring->base);
    assert(ring->base_paddr);

    ring->dequeue = ring->base;
    ring->dequeue_paddr = ring->base_paddr;

    if (size > 4096) {
        size_t curr = PAGE_SIZE_DWORDS;
        uintptr_t prev_paddr = ring->base_paddr;
        uint32_t* base = ring->base;

        // Insert Link TRBs
        size_t page_count = (size + 4095) / 4096;
        FOR_N(i, 1, page_count) {
            uintptr_t paddr = syscall(SYS_get_paddr, &base[i*PAGE_SIZE_DWORDS]);
            if (prev_paddr+4096 != paddr) {
                // Not contiguous? insert link at the end of the prev_paddr
                uint32_t* link_entry = &base[i*PAGE_SIZE_DWORDS - 4];
                link_entry[0] = paddr & 0xFFFFFFF0;
                link_entry[1] = paddr >> 32ull;
                link_entry[3] = 6u << 10u;
            }
            prev_paddr = paddr;
        }

        // Final Link TRB
        uint32_t* link_entry = &base[(size - 16) / 4];
        link_entry[0] = ring->base_paddr & 0xFFFFFFF0;
        link_entry[1] = ring->base_paddr >> 32ull;
        link_entry[3] = (6u << 10u) | 2u; // Toggle cycle
    } else {
        // Final Link TRB
        uint32_t* link_entry = &ring->base[(size - 16) / 4];
        link_entry[0] = ring->base_paddr & 0xFFFFFFF0;
        link_entry[1] = ring->base_paddr >> 32ull;
        link_entry[3] = (6u << 10u) | 2u; // Toggle cycle
    }
}

static uint32_t* ring_cmd_at(HCI_Ring* ring, uintptr_t paddr) {
    return &ring->base[(paddr - ring->base_paddr) / 4];
}

static void ring_advance(HCI_Ring* ring) {
    // Move 16B forward
    ring->dequeue += 4;
    ring->dequeue_paddr += 16;

    // Check for Link TRBs
    uint32_t peek = ring->dequeue[3];
    uint32_t type = (peek >> 10u) & 0b111111;
    if (type == 6) {
        // Link TRB should update the cycle bit
        ring->dequeue[3] = (6u << 10u) | 2u | (ring->cycle_bit & 1);

        // Follow link
        ring->dequeue_paddr = ring->dequeue[0] | ((uintptr_t) ring->dequeue[1] << 32ull);
        ring->dequeue       = ring->base + ((ring->dequeue_paddr - ring->base_paddr) / sizeof(uint32_t));

        // Toggle Cycle
        if ((peek >> 1) & 1) {
            ring->cycle_bit = !ring->cycle_bit;
        }
    }
}

static void ring_submit_cmd2(HCI_Ring* ring, int type, uint32_t cmd[4]) {
    uint32_t* dst = ring->dequeue;

    // copy command (except control field)
    FOR_N(i, 0, 3) { dst[i] = cmd[i]; }
    // don't toggle cycle bit just yet
    dst[3] = cmd[3] | ((type & 0x3F) << 10u) | !ring->cycle_bit;

    // printf("SUBMIT[%p] %08x %08x %08x %08x\n", ring->dequeue_paddr, dst[0], dst[1], dst[2], dst[3]);
    ring_advance(ring);
}

static void ring_submit_cmd(HCI_Ring* ring, int type, uint32_t cmd[4]) {
    uint32_t* dst = ring->dequeue;

    // copy command (except control field)
    FOR_N(i, 0, 3) { dst[i] = cmd[i]; }
    // write out, last word in the command must be written last since
    // it holds the control field and the producer cycle bit.
    dst[3] = cmd[3] | ((type & 0x3F) << 10u) | ring->cycle_bit;
    asm volatile("mfence" : : : "memory");

    // printf("SUBMIT[%p] %08x %08x %08x %08x\n", ring->dequeue_paddr, dst[0], dst[1], dst[2], dst[3]);
    ring_advance(ring);
}

static void ring_dump_cmd(uintptr_t paddr, uint32_t* cmd) {
    printf("PEEK  [%p] %08x %08x %08x %08x\n", paddr, cmd[0], cmd[1], cmd[2], cmd[3]);
}

