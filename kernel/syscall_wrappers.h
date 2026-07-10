typedef void* Rawptr;

static uintptr_t syscall_test(CPUState* state, uintptr_t cr3, PerCPU* cpu, uint64_t a0);
static uintptr_t SYSW_test(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    uint64_t        arg_0 = (uint64_t) SYS_PARAM0;
    ON_DEBUG(SYSCALL)(kprintf("SYS_test(%p)\n", SYS_PARAM0));
    return syscall_test(state, cr3, cpu, arg_0);
}

static uintptr_t syscall_debug_log(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, size_t a1);
static uintptr_t SYSW_debug_log(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    size_t          arg_1 = (size_t) SYS_PARAM1;
    ON_DEBUG(SYSCALL)(kprintf("SYS_debug_log(%p, %p)\n", SYS_PARAM0, SYS_PARAM1));
    return syscall_debug_log(state, cr3, cpu, arg_0, arg_1);
}

static uintptr_t syscall_thread_create(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, int a5);
static uintptr_t SYSW_thread_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    uintptr_t       arg_1 = (uintptr_t) SYS_PARAM1;
    uintptr_t       arg_2 = (uintptr_t) SYS_PARAM2;
    uintptr_t       arg_3 = (uintptr_t) SYS_PARAM3;
    uintptr_t       arg_4 = (uintptr_t) SYS_PARAM4;
    int             arg_5 = (int) SYS_PARAM5;
    ON_DEBUG(SYSCALL)(kprintf("SYS_thread_create(%p, %p, %p, %p, %p, %p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3, SYS_PARAM4, SYS_PARAM5));
    return syscall_thread_create(state, cr3, cpu, arg_0, arg_1, arg_2, arg_3, arg_4, arg_5);
}

static uintptr_t syscall_thread_control(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, int a1, uint64_t a2, Rawptr a3);
static uintptr_t SYSW_thread_control(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    int             arg_1 = (int) SYS_PARAM1;
    uint64_t        arg_2 = (uint64_t) SYS_PARAM2;
    Rawptr          arg_3 = (Rawptr) SYS_PARAM3;
    ON_DEBUG(SYSCALL)(kprintf("SYS_thread_control(%p, %p, %p, %p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3));
    return syscall_thread_control(state, cr3, cpu, arg_0, arg_1, arg_2, arg_3);
}

static uintptr_t syscall_env_create(CPUState* state, uintptr_t cr3, PerCPU* cpu);
static uintptr_t SYSW_env_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_env_create()\n"));
    return syscall_env_create(state, cr3, cpu);
}

static uintptr_t syscall_mem_map(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, MapControl a1, KHandle a2, uintptr_t a3, size_t a4, int a5);
static uintptr_t SYSW_mem_map(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    MapControl      arg_1; memcpy(&arg_1, &SYS_PARAM1, sizeof(MapControl));
    KHandle         arg_2 = (KHandle) SYS_PARAM2;
    uintptr_t       arg_3 = (uintptr_t) SYS_PARAM3;
    size_t          arg_4 = (size_t) SYS_PARAM4;
    int             arg_5 = (int) SYS_PARAM5;
    ON_DEBUG(SYSCALL)(kprintf("SYS_mem_map(%p, %p, %p, %p, %p, %p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3, SYS_PARAM4, SYS_PARAM5));
    return syscall_mem_map(state, cr3, cpu, arg_0, arg_1, arg_2, arg_3, arg_4, arg_5);
}

static uintptr_t syscall_mem_unmap(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, uintptr_t a1, size_t a2);
static uintptr_t SYSW_mem_unmap(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    uintptr_t       arg_1 = (uintptr_t) SYS_PARAM1;
    size_t          arg_2 = (size_t) SYS_PARAM2;
    ON_DEBUG(SYSCALL)(kprintf("SYS_mem_unmap(%p, %p, %p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));
    return syscall_mem_unmap(state, cr3, cpu, arg_0, arg_1, arg_2);
}

static uintptr_t syscall_mem_translate(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, Rawptr a1);
static uintptr_t SYSW_mem_translate(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    Rawptr          arg_1 = (Rawptr) SYS_PARAM1;
    ON_DEBUG(SYSCALL)(kprintf("SYS_mem_translate(%p, %p)\n", SYS_PARAM0, SYS_PARAM1));
    return syscall_mem_translate(state, cr3, cpu, arg_0, arg_1);
}

static uintptr_t syscall_vmo_create(CPUState* state, uintptr_t cr3, PerCPU* cpu, size_t a0);
static uintptr_t SYSW_vmo_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    size_t          arg_0 = (size_t) SYS_PARAM0;
    ON_DEBUG(SYSCALL)(kprintf("SYS_vmo_create(%p)\n", SYS_PARAM0));
    return syscall_vmo_create(state, cr3, cpu, arg_0);
}

static uintptr_t syscall_vmo_get_size(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0);
static uintptr_t SYSW_vmo_get_size(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    ON_DEBUG(SYSCALL)(kprintf("SYS_vmo_get_size(%p)\n", SYS_PARAM0));
    return syscall_vmo_get_size(state, cr3, cpu, arg_0);
}

static uintptr_t syscall_event_create(CPUState* state, uintptr_t cr3, PerCPU* cpu);
static uintptr_t SYSW_event_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_event_create()\n"));
    return syscall_event_create(state, cr3, cpu);
}

static uintptr_t syscall_event_op(CPUState* state, uintptr_t cr3, PerCPU* cpu, int a0, KHandle a1);
static uintptr_t SYSW_event_op(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    int             arg_0 = (int) SYS_PARAM0;
    KHandle         arg_1 = (KHandle) SYS_PARAM1;
    ON_DEBUG(SYSCALL)(kprintf("SYS_event_op(%p, %p)\n", SYS_PARAM0, SYS_PARAM1));
    return syscall_event_op(state, cr3, cpu, arg_0, arg_1);
}

static uintptr_t syscall_pci_peek_device(CPUState* state, uintptr_t cr3, PerCPU* cpu, size_t a0, Rawptr a1);
static uintptr_t SYSW_pci_peek_device(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    size_t          arg_0 = (size_t) SYS_PARAM0;
    Rawptr          arg_1 = (Rawptr) SYS_PARAM1;
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_peek_device(%p, %p)\n", SYS_PARAM0, SYS_PARAM1));
    return syscall_pci_peek_device(state, cr3, cpu, arg_0, arg_1);
}

static uintptr_t syscall_pci_claim_device(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, Rawptr a1);
static uintptr_t SYSW_pci_claim_device(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    Rawptr          arg_1 = (Rawptr) SYS_PARAM1;
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_claim_device(%p, %p)\n", SYS_PARAM0, SYS_PARAM1));
    return syscall_pci_claim_device(state, cr3, cpu, arg_0, arg_1);
}

static uintptr_t syscall_pci_read_config_32(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, size_t a1);
static uintptr_t SYSW_pci_read_config_32(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    size_t          arg_1 = (size_t) SYS_PARAM1;
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_read_config_32(%p, %p)\n", SYS_PARAM0, SYS_PARAM1));
    return syscall_pci_read_config_32(state, cr3, cpu, arg_0, arg_1);
}

static uintptr_t syscall_pci_write_config_32(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, size_t a1, uint32_t a2);
static uintptr_t SYSW_pci_write_config_32(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    size_t          arg_1 = (size_t) SYS_PARAM1;
    uint32_t        arg_2 = (uint32_t) SYS_PARAM2;
    ON_DEBUG(SYSCALL)(kprintf("SYS_pci_write_config_32(%p, %p, %p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2));
    return syscall_pci_write_config_32(state, cr3, cpu, arg_0, arg_1, arg_2);
}

static uintptr_t syscall_mailbox_create(CPUState* state, uintptr_t cr3, PerCPU* cpu);
static uintptr_t SYSW_mailbox_create(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_create()\n"));
    return syscall_mailbox_create(state, cr3, cpu);
}

static uintptr_t syscall_mailbox_ipc(CPUState* state, uintptr_t cr3, PerCPU* cpu, KHandle a0, KHandle a1, MSG_Tag a2, KHandle a3);
static uintptr_t SYSW_mailbox_ipc(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    KHandle         arg_0 = (KHandle) SYS_PARAM0;
    KHandle         arg_1 = (KHandle) SYS_PARAM1;
    MSG_Tag         arg_2; memcpy(&arg_2, &SYS_PARAM2, sizeof(MSG_Tag));
    KHandle         arg_3 = (KHandle) SYS_PARAM3;
    ON_DEBUG(SYSCALL)(kprintf("SYS_mailbox_ipc(%p, %p, %p, %p)\n", SYS_PARAM0, SYS_PARAM1, SYS_PARAM2, SYS_PARAM3));
    return syscall_mailbox_ipc(state, cr3, cpu, arg_0, arg_1, arg_2, arg_3);
}

static uintptr_t syscall_root_mailbox(CPUState* state, uintptr_t cr3, PerCPU* cpu);
static uintptr_t SYSW_root_mailbox(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_root_mailbox()\n"));
    return syscall_root_mailbox(state, cr3, cpu);
}

static uintptr_t syscall_tsc_freq(CPUState* state, uintptr_t cr3, PerCPU* cpu);
static uintptr_t SYSW_tsc_freq(CPUState* state, uintptr_t cr3, PerCPU* cpu) {
    ON_DEBUG(SYSCALL)(kprintf("SYS_tsc_freq()\n"));
    return syscall_tsc_freq(state, cr3, cpu);
}


SyscallFn* syscall_table[] = {
    [SYS_test] = SYSW_test,
    [SYS_debug_log] = SYSW_debug_log,
    [SYS_thread_create] = SYSW_thread_create,
    [SYS_thread_control] = SYSW_thread_control,
    [SYS_env_create] = SYSW_env_create,
    [SYS_mem_map] = SYSW_mem_map,
    [SYS_mem_unmap] = SYSW_mem_unmap,
    [SYS_mem_translate] = SYSW_mem_translate,
    [SYS_vmo_create] = SYSW_vmo_create,
    [SYS_vmo_get_size] = SYSW_vmo_get_size,
    [SYS_event_create] = SYSW_event_create,
    [SYS_event_op] = SYSW_event_op,
    [SYS_pci_peek_device] = SYSW_pci_peek_device,
    [SYS_pci_claim_device] = SYSW_pci_claim_device,
    [SYS_pci_read_config_32] = SYSW_pci_read_config_32,
    [SYS_pci_write_config_32] = SYSW_pci_write_config_32,
    [SYS_mailbox_create] = SYSW_mailbox_create,
    [SYS_mailbox_ipc] = SYSW_mailbox_ipc,
    [SYS_root_mailbox] = SYSW_root_mailbox,
    [SYS_tsc_freq] = SYSW_tsc_freq,
};
size_t syscall_table_count = SYS_MAX;
