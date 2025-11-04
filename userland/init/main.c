#include <beans.h>

int _start(KHandle bootstrap_vmo) {

    // Find the first set of connected PCI devices
    /* int dev_count = syscall(SYS_pci_device_count);
    for (int i = 0; i < dev_count; i++) {
        uint32_t key;
        KHandle dev = syscall(SYS_pci_device_count, i, &key);

        if (key == ) {

        }
    } */

    for (;;) {
        syscall(SYS_test, 0);
        syscall(SYS_sleep, 100000);
    }
}

