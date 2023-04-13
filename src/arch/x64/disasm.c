
enum {
    X86_OP_DIR   = 1, // if set, we go from r/m, reg => reg, r/m
    X86_OP_MODRM = 2,
    X86_OP_IMM   = 4,
};

enum {
    MOD_INDIRECT = 0,        // [rax]
    MOD_INDIRECT_DISP8 = 1,  // [rax + disp8]
    MOD_INDIRECT_DISP32 = 2, // [rax + disp32]
    MOD_DIRECT = 3,          // rax
};

typedef struct {
    const char* name;
    u8 flags;
} Opcode;

static const Opcode opcode_table[] = {
    [0x89] = { "mov", X86_OP_MODRM },
    [0x8B] = { "mov", X86_OP_MODRM | X86_OP_DIR },
};

#define DECODE_MODRXRM(mod, rx, rm, src) \
(mod = (src >> 6) & 3, rx = (src >> 3) & 7, rm = (src & 7))

static const char* X86_GPR_NAMES[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* x86_prefix_name(uint8_t b) {
    switch (b) {
        case 0x2E: return "CS ";
        case 0x36: return "SS ";
        case 0x3E: return "DS ";
        case 0x26: return "ES ";
        case 0x64: return "FS ";
        case 0x65: return "GS ";
        case 0xF0: return "LOCK ";
        default:   return NULL;
    }
}

static void x86_print_disasm(u8* p, size_t size) {
    u8* end = &p[size];
    while (p < end) {
        // print raw
        kprintf("  ");
        FOREACH_N(i, 0, 16) {
            kprintf("0x%x ", p[i]);
        }
        kprintf("\n");

        kprintf("  ");

        // prefixes
        const char* n;
        for (; (n = x86_prefix_name(*p)) != NULL; p++) {
            kprintf(n);
        }

        // REX
        if ((*p & 0xF0) == 0x40) {
            kprintf("REX");
            if (*p & 8) kprintf(".W");
            kprintf(" ");

            p++;
        }

        // opcode
        Opcode* op = &opcode_table[*p++];
        if (op->name) {
            kprintf("%s ", op->name);
        } else {
            kprintf("%x ", p[-1]);
        }

        if (op->flags & X86_OP_MODRM) {
            // modrm+sib
            u8 mod, rx, rm;
            DECODE_MODRXRM(mod, rx, rm, *p++);

            if (op->flags & X86_OP_DIR) {
                kprintf("%s", X86_GPR_NAMES[rx]);
            }

            if (mod < MOD_DIRECT) {
                kprintf("TODO MEMORY OPERAND\n");
                // kprintf(X86_GPR_NAMES[]);
            } else {
                kprintf("%s ", X86_GPR_NAMES[rm]);
            }

            if ((op->flags & X86_OP_DIR) == 0) {
                kprintf("%s", X86_GPR_NAMES[rx]);
            }

            // displacement
            if (mod == MOD_INDIRECT_DISP8) {
                kprintf("%d ", *p++);
            } else if (mod == MOD_INDIRECT_DISP32) {
                i32 disp;
                memcpy(&disp, p, 4), p += 4;
                kprintf("%d ", (i64) disp);
            }
        }

        // immediate
        if (op->flags & X86_OP_IMM) {
        }

        kprintf("\n");
    }
}
