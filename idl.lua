prelude = require "prelude"
inspect = require "inspect"

local source = run_command("clang -E -xc beans.dsl")
local tree = parse_node(lexer(source))

local lines = {}

lines[#lines + 1] = "typedef void* Rawptr;"
lines[#lines + 1] = ""

local prims = {
    "int", "size_t", "uintptr_t", "KHandle", "Rawptr",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
}

local to_prim = {}
for i=1,#prims do
    to_prim[prims[i]] = true
end

-- generate wrappers
for i=1,#tree do
    if tree[i][1] == "syscall" then
        -- generate C wrapper impls
        local name = tree[i][2]
        local ret  = tree[i][3]
        local args = tree[i][4]

        local proto = {}
        for j=1,#args do
            proto[#proto + 1] = string.format(", %s a%d", args[j], j-1)
        end

        lines[#lines + 1] = string.format("static uintptr_t syscall_%s(CPUState* state, uintptr_t cr3, PerCPU* cpu%s);", name, table.concat(proto))
        lines[#lines + 1] = string.format("static uintptr_t SYSW_%s(CPUState* state, uintptr_t cr3, PerCPU* cpu) {", name)
        -- swizzle inputs into shape
        local log_str = {}
        local args_str = {}
        local fmt_str = {}
        for j=1,#args do
            if to_prim[args[j]] then
                lines[#lines + 1] = string.format("    %-15s arg_%d = (%s) SYS_PARAM%d;", args[j], j - 1, args[j], j - 1)
            else
                lines[#lines + 1] = string.format("    %-15s arg_%d; memcpy(&arg_%d, &SYS_PARAM%d, sizeof(%s));", args[j], j - 1, j - 1, j - 1, args[j])
            end
            args_str[j] = string.format(", arg_%d", j-1)
            log_str[j] = string.format(", SYS_PARAM%d", j-1)
            fmt_str[j] = "%p"
        end
        lines[#lines + 1] = string.format("    ON_DEBUG(SYSCALL)(kprintf(\"SYS_%s(%s)\"%s));", name, table.concat(fmt_str, ", "), table.concat(log_str))
        lines[#lines + 1] = string.format("    return syscall_%s(state, cr3, cpu%s);", name, table.concat(args_str))
        lines[#lines + 1] = "}"
        lines[#lines + 1] = ""

        print("// " .. inspect(tree[i]))
    end
end

lines[#lines + 1] = ""
lines[#lines + 1] = "SyscallFn* syscall_table[] = {"
for i=1,#tree do
    if tree[i][1] == "syscall" then
        local name = tree[i][2]
        lines[#lines + 1] = string.format("    [SYS_%s] = SYSW_%s,", name, name)
    end
end
lines[#lines + 1] = "};"
lines[#lines + 1] = "size_t syscall_table_count = SYS_MAX;"
lines[#lines + 1] = ""

f = io.open("kernel/syscall_wrappers.h", "w")
f:write(table.concat(lines, "\n"))
f:close()

lines = {}
for i=1,#tree do
    if tree[i][1] == "syscall" then
        local name = tree[i][2]
        lines[#lines + 1] = string.format("X(%s)", name)
    end
end
lines[#lines + 1] = "#undef X"
lines[#lines + 1] = ""

f = io.open("include/syscall_table.h", "w")
f:write(table.concat(lines, "\n"))
f:close()
