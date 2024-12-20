
#include <stdbool.h>
#include <stdarg.h>

#define PORT_COM1 0x03F8
#define PORT_COM2 0x02F8
#define PORT_COM3 0x03E8
#define PORT_COM4 0x02E8
#define PORT_COM5 0x05F8
#define PORT_COM6 0x04F8
#define PORT_COM7 0x05E8
#define PORT_COM8 0x04E8

#define COM_PO_DR  +0 // Data Register
#define COM_PO_IER +1 // Interrupt Enable Register
#define COM_PO_FCR +2 // Interrupt ID and FIFO Control Register
#define COM_PO_LCR +3 // Line Control Register
#define COM_PO_MCR +4 // Modem Control Register
#define COM_PO_LSR +5 // Line Status Register
#define COM_PO_MSR +6 // Modem Status Regsiter
#define COM_PO_SR  +7 // Scratch Register

#define COM_PO_BAUD_LO +0
#define COM_PO_BAUD_HI +1

#define COM_DEFAULT_BAUD 115200

static u16 com_port = PORT_COM1;

// MSVC+Clang intrin
u8 io_in8(u16 port) {
    u8 value;
    asm volatile(".intel_syntax noprefix; in %b0, %w1" : "=a" (value) : "Nd" (port));
    return value;
}

void io_out8(u16 port, u8 data) {
    asm volatile(".intel_syntax noprefix; out %w1, %b0" : : "a" (data), "Nd" (port));
}

static inline void com_set_port(u16 port) {
    com_port = port;
}

static inline bool com_init(int port, u32 baud) {
    u16 divisor = COM_DEFAULT_BAUD / baud;
    u8 divisor_lo = (u8)(divisor & 0xff);
    u8 divisor_hi = (u8)((divisor>>8) & 0xff);
    io_out8(port+COM_PO_IER, 0x00); // Disable all interrupts
    io_out8(port+COM_PO_LCR, 0x80); // Enable DLAB to set baud rate divisor
    io_out8(port+COM_PO_BAUD_LO, divisor_lo);
    io_out8(port+COM_PO_BAUD_HI, divisor_hi);
    io_out8(port+COM_PO_LCR, 0x03); // 8 bits per character
    io_out8(port+COM_PO_FCR, 0xc7); // Enable FIFO, do some shit
    io_out8(port+COM_PO_MCR, 0x0b); // RTS/DSR set
    // Test the chip
    io_out8(port+COM_PO_MCR, 0x1e); // Set in loopback mode
    io_out8(port+COM_PO_DR, 0xae); // Write 0xae
    if(io_in8(port+COM_PO_DR) != 0xae) {
        return false;
    }
    io_out8(port+COM_PO_MCR, 0x0f); // Set back to normal mode
    return true;
}

static inline bool com_read_ready() {
    return io_in8(com_port+COM_PO_LSR) & 1;
}

static inline bool com_write_ready() {
    return (io_in8(com_port+COM_PO_MSR) & 0x20) == 0x20;
}

static inline u8 com_read8() {
    while(!com_read_ready()) {}
    return io_in8(com_port);
}

static inline void com_write8(u8 ch) {
    while(!com_write_ready()) {}
    return io_out8(com_port, ch);
}

static inline void com_writec(char ch) {
    com_write8((char)ch);
}

static inline void com_writes(char* str) {
    while(*str) {
        com_writec(*str++);
    }
}
