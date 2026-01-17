X(sleep)
X(test)
// Env/Threads
X(env_create)
X(thread_create)
// Tracing/Debug
X(debug_log)
// Pipe
X(pipe_create)
X(pipe_send)
X(pipe_recv)
// VMM
X(mmap)
X(munmap)
X(mpin)
X(mdump)
X(vmo_create)
X(vmo_get_size)
// PCI
X(pci_device_count)
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
// Special case
X(fb_grab)
// X(ps2_interrupt)
// x86 stuff
X(tsc_freq)
#undef X
