X(sleep)
X(sched_time)
X(test)
// Namespace
X(get_root_mailbox)
// Env/Threads
X(env_create)
X(thread_create)
X(thread_setattr)
// Tracing/Debug
X(debug_log)
// Event
X(event_create)
X(event_wait)
X(event_signal)
// VMM
X(mmap)
X(munmap)
X(mpin)
X(mdump)
X(get_paddr)
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
// x86 stuff
X(tsc_freq)
#undef X
