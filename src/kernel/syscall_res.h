enum {
    RESULT_SUCCESS =  0,

    RESULT_NO_MEM  = -1,

    // PCI errors
    RESULT_NO_BAR  = -2, // the BAR you're reading isn't there (OOB?)
    RESULT_IO_BAR  = -3, // you tried to map an I/O BAR into memory

    // Handle errors
    RESULT_NO_HANDLE    = -4, // Handle was 0
    RESULT_WRONG_HANDLE = -5, // Type mismatch
};

