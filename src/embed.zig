
const Buffer = extern struct {
    length: usize,
    data: [*c]const u8,
};

fn getFile(comptime path: []const u8) Buffer {
    comptime var b = @embedFile(path);
    return .{ .length = b.len, .data = &b[0] };
}

export var test2 = getFile("test2.elf");
