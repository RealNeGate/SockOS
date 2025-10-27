X(sleep)
X(test)
X(thread_create)
// VMM
X(mmap)
X(munmap)
// Special case
X(fb_grab)
// PCI
X(pci_claim_device)
X(pci_bar_count)
X(pci_get_bar)
X(pci_read_config_32)
X(pci_write_config_32)
// RPC
X(mailbox_create)
X(mailbox_send)
X(mailbox_wait)
X(mailbox_reply)
#undef X
