const std = @import("std");
const CrossTarget = std.zig.CrossTarget;
const FileSource = std.Build.FileSource;

pub fn build(b: *std.Build) void {
    const optimize = b.standardOptimizeOption(.{});

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

    const kernel = b.addExecutable(.{
            .name = "kernel.so",
            .target = .{ .cpu_arch = .x86_64, .os_tag = .linux, .abi = .gnu },
            .optimize = optimize,
        });

    kernel.force_pic = true;
    kernel.pie = true;
    kernel.setLinkerScriptPath(FileSource.relative("src/kernel/link"));
    kernel.setOutputDir("bin");
    kernel.addIncludePath("src");
    kernel.addCSourceFile("src/kernel/kernel.c", &kernel_cflags);
    kernel.addAssemblyFile("src/arch/x64/loader.s");
    kernel.addAssemblyFile("src/arch/x64/irq.s");
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
        "-smp", "cores=4,threads=1,sockets=1"
    };

    const run_cmd = b.addSystemCommand(qemu_cmd);
    run_cmd.step.dependOn(b.getInstallStep());

    const run_step = b.step("run", "Run with QEMU");
    run_step.dependOn(&run_cmd.step);
}
