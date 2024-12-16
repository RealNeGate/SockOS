#include <kernel.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define PS2_ENABLE_PORT1 0xAE
#define PS2_ENABLE_PORT2 0xA8

#define PS2_DISABLE_PORT1 0xAD
#define PS2_DISABLE_PORT2 0xA7

#define PS2_READ_CONFIG  0x20
#define PS2_WRITE_CONFIG 0x60

#define PS2_PORT1_IRQ 0b01
#define PS2_PORT2_IRQ 0b10

void ps2_wait_input(void) {
    while ((io_in8(PS2_STATUS) & 0b10));
    return;
}

void ps2_wait_output(void) {
    while (!(io_in8(PS2_STATUS) & 0b01));
    return;
}

void ps2_cmd(u8 cmd) {
    ps2_wait_input();
    io_out8(PS2_COMMAND, cmd);
}

u8 ps2_cmd_resp(u8 cmd) {
    ps2_wait_input();
    io_out8(PS2_COMMAND, cmd);
    ps2_wait_output();
    return io_in8(PS2_DATA);
}

void ps2_cmd_arg(u8 cmd, u8 arg) {
    ps2_wait_input();
    io_out8(PS2_COMMAND, cmd);
    ps2_wait_input();
    io_out8(PS2_DATA, arg);
}

char key_map[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    '\a', 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', '\a',
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

void x86_halt();
void ps2_interrupt(void* ctx) {
    u8 key = io_in8(PS2_DATA);

    if ((key & 0x80) == 0) {
        _putchar(key_map[key]);
    }

    // kprintf("HEHEHE! %d\n", key);
}

void ps2_init(void) {
    // Read the PS2 Controller Config
    ps2_cmd(PS2_DISABLE_PORT1);
    ps2_cmd(PS2_DISABLE_PORT2);

    u8 status = ps2_cmd_resp(PS2_READ_CONFIG);
    status |= (PS2_PORT1_IRQ | PS2_PORT2_IRQ);
    ps2_cmd_arg(PS2_WRITE_CONFIG, status);

    set_interrupt_line(1, ps2_interrupt, NULL);

    ps2_cmd(PS2_ENABLE_PORT1);
    ps2_cmd(PS2_ENABLE_PORT2);
}
