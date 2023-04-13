const builtin = @import("builtin");
const std = @import("std");
const CrossTarget = std.zig.CrossTarget;
const FileSource = std.Build.FileSource;
const path = std.fs.path;

fn nasm(b: *std.Build, obj: *std.build.CompileStep, comptime file: []const u8) []const u8 {
    comptime var out_file = "zig-cache/" ++ path.basename(file) ++ ".o";
    var cmd = b.addSystemCommand(&[_][]const u8{
            "nasm", "-f", "elf64", file, "-o", out_file
        });

    obj.step.dependOn(&cmd.step);
    return out_file;
}

fn user_program(b: *std.Build, obj: *std.build.CompileStep, comptime file: []const u8) void {
    const cflags = [_][]const u8{
        "-Wall", "-Wno-unused", "-fno-stack-protector", "-fno-PIC",
        "-fshort-wchar", "-mno-red-zone", "-fno-finite-loops",
    };

    const optimize = std.builtin.Mode.Debug;
    const cmd = b.addExecutable(.{
            .name = file ++ ".elf",
            .target = .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
            .optimize = optimize,
        });

    cmd.disable_sanitize_c = true;
    cmd.setOutputDir("src/");
    cmd.addCSourceFile("userland/" ++ file ++ ".c", &cflags);
    cmd.install();

    obj.step.dependOn(&cmd.step);
}

pub fn build(b: *std.Build) void {
    // const optimize = b.standardOptimizeOption(.{});
    const optimize = std.builtin.Mode.Debug;

    ////////////////////////////////
    // Build EFI app
    ////////////////////////////////
    const efi_cflags = [_][]const u8{
        "-Wall", "-Wno-unused", "-fno-stack-protector", "-fno-PIC",
        "-fshort-wchar", "-mno-red-zone", "-fno-finite-loops",
    };

    const efi = b.addExecutable(.{
            .name = "bootx64",
            .target = .{ .cpu_arch = .x86_64, .os_tag = .uefi, .abi = .msvc },
            .optimize = optimize,
        });

    efi.disable_sanitize_c = true;
    efi.setOutputDir("bin/efi/boot");
    efi.subsystem = .EfiApplication;
    efi.addIncludePath("src");
    efi.addCSourceFile("src/boot/efi_main.c", &efi_cflags);
    efi.install();

    ////////////////////////////////
    // Build Kernel
    ////////////////////////////////
    const kernel_cflags = [_][]const u8{
        "-Wall", "-Wno-unused", "-fno-stack-protector",
        "-mno-red-zone", "-fno-finite-loops", "-ffreestanding",
        "-masm=intel"
    };

    const embedded = b.addObject(.{
            .name = "embed.o",
            .root_source_file = .{ .path = "src/embed.zig" },
            .target = .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
            .optimize = optimize,
        });
    embedded.addIncludePath(".");
    user_program(b, embedded, "desktop");

    const kernel = b.addExecutable(.{
            .name = "kernel.so",
            .target = .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
            .optimize = optimize,
        });

    kernel.force_pic = true;
    kernel.pie = true;
    kernel.disable_sanitize_c = true;
    kernel.setLinkerScriptPath(FileSource.relative("src/kernel/link"));
    kernel.setOutputDir("bin");
    kernel.addIncludePath("src");
    kernel.addObject(embedded);
    kernel.addCSourceFile("src/kernel/kernel.c", &kernel_cflags);
    kernel.addAssemblyFile("src/arch/x64/loader.s");
    kernel.addAssemblyFile("src/arch/x64/irq.s");
    kernel.addObjectFile(nasm(b, kernel, "src/arch/x64/bootstrap.s"));
    kernel.install();

    ////////////////////////////////
    // Qemu testing
    ////////////////////////////////
    const qemu_cmd = &[_][]const u8{
        "qemu-system-x86_64",
        "-serial", "stdio",
        "-bios", "OVMF.fd",
        "-drive", "format=raw,file=fat:rw:bin",
        "-no-reboot", "-s",
        "-d", "int", "-D", "qemu.log",
        "-m", "256M",
        "-smp", "cores=4,threads=1,sockets=1"
    };

    const run_cmd = b.addSystemCommand(qemu_cmd);
    run_cmd.step.dependOn(b.getInstallStep());

    const run_step = b.step("run", "Run with QEMU");
    run_step.dependOn(&run_cmd.step);
}
