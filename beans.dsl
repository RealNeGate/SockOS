(syscall test           void    (uint64_t))

// Debug Tracing
//                                 vmo     size
(syscall debug_log      void      (KHandle size_t))

// Allocate new thread
//                                 env     ip        sp         arg       utcb      flags
(syscall thread_create  KHandle   (KHandle uintptr_t uintptr_t  uintptr_t uintptr_t int))

// Set thread state or send async signals (resume, pause). You can also perform
// self-signalling such as exit or join (rquire thread=0).
//                                 thread  request   arg       ptr
(syscall thread_control void      (KHandle int       uint64_t  Rawptr))

// Allocate new environment/address space
(syscall env_create     KHandle   ())

// Allocate or Change the permissions on a page range
//                                 env     ctrl       vmo       offset    size   flags
(syscall mem_map        Rawptr    (KHandle MapControl KHandle   uintptr_t size_t int))
// Unmap page range it can now be re-allocated by later mem_map calls.
//                                 env     addr      size
(syscall mem_unmap      int       (KHandle uintptr_t size_t))

// Return the translated page
//                                 env     addr
(syscall mem_translate  uintptr_t (KHandle Rawptr))

// Creates a region of virtual memory which can be shared
//                                 size
(syscall vmo_create     KHandle   (size_t))
(syscall vmo_get_size   size_t    (KHandle))

// Events
(syscall event_create   KHandle   ())
(syscall event_wait     int       (KHandle))
(syscall event_signal   int       (KHandle))

// PCI
(syscall pci_peek_device     KHandle  (size_t  Rawptr))
(syscall pci_claim_device    KHandle  (KHandle Rawptr))
(syscall pci_read_config_32  uint32_t (KHandle size_t))
(syscall pci_write_config_32 void     (KHandle size_t uint32_t))

(syscall mailbox_create KHandle   (int))
(syscall mailbox_send   uintptr_t ())
(syscall mailbox_wait   uintptr_t ())
(syscall mailbox_reply  uintptr_t ())

// Namespace
(syscall root_mailbox   KHandle ())

// TODO(NeGate): get rid of this later
(syscall tsc_freq       uint64_t ())
